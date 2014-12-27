#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <inttypes.h>
#include <assert.h>
#include <math.h>
#include <sys/time.h>
#include "hmmcons_poremodel.h"
#include "hmmcons_interface.h"

// Constants

// strands
const uint8_t T_IDX = 0;
const uint8_t C_IDX = 1;
const uint8_t NUM_STRANDS = 2;

// 
const uint8_t K = 5;

static const uint8_t base_rank[256] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,1,0,0,0,2,0,0,0,0,0,0,0,0,
    0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

//#define DEBUG_HMM_UPDATE 1
//#define DEBUG_HMM_EMISSION 1

struct CEventSequence
{
    uint32_t n_events;
    const double* level;
};

struct CSquiggleRead
{
    // unique identifier of the read
    uint32_t read_id;

    // one model for each strand
    CPoreModel pore_model[2];

    // one event sequence for each strand as well
    CEventSequence events[2];
};

struct HMMConsReadState
{
    uint32_t read_idx;
    uint32_t event_idx;
    uint32_t kmer_idx;
    uint8_t strand;
    uint8_t stride;
    std::string alignment;
};

struct ExtensionResult
{
    double full_path[1024];
    double next_kmer[4];
};

// A global vector used to store data we've received from the python code
struct HmmConsData
{
    std::vector<CSquiggleRead> reads;
    std::vector<HMMConsReadState> read_states;
};
HmmConsData g_data;

extern "C"
void add_read(CSquiggleReadInterface params)
{
    g_data.reads.push_back(CSquiggleRead());
    CSquiggleRead& sr = g_data.reads.back();
 
    for(uint32_t i = 0; i < NUM_STRANDS; ++i) {
        // Initialize pore model   
        sr.pore_model[i].scale = params.pore_model[i].scale;
        sr.pore_model[i].shift = params.pore_model[i].shift;
        
        assert(params.pore_model[i].n_states == 1024);
        for(uint32_t j = 0; j < params.pore_model[i].n_states; ++j) {
            sr.pore_model[i].state[j].mean = params.pore_model[i].mean[j];
            sr.pore_model[i].state[j].sd = params.pore_model[i].sd[j];
         }
    
        // Initialize events
        sr.events[i].n_events = params.events[i].n_events;
        sr.events[i].level = params.events[i].level;
        
        /*
        printf("Model[%zu] scale: %lf shift: %lf %lf %lf\n", i, sr.pore_model[i].scale, 
                                                                 sr.pore_model[i].shift,
                                                                 sr.pore_model[i].state[0].mean, 
                                                                 sr.pore_model[i].state[0].sd);
    
        printf("First 100 events of %d\n", sr.events[i].n_events);
        for(int j = 0; j < 100; ++j)
            printf("%d: %lf\n", j, sr.events[i].level[j]);
        */
    }
}

extern "C"
void add_read_state(CReadStateInterface params)
{
    g_data.read_states.push_back(HMMConsReadState());
    HMMConsReadState& rs = g_data.read_states.back();
    rs.read_idx = params.read_idx;
    rs.event_idx = params.event_idx;
    rs.kmer_idx = 0;
    rs.strand = params.strand;
    rs.stride = params.stride;
}

//
// HMM matrix
//
struct HMMCell
{
    double M;
    double E;
    double K;
};

inline uint32_t cell(uint32_t row, uint32_t col, uint32_t n_cols)
{
    return row * n_cols + col;
}

inline uint32_t kmer_rank(const char* str, uint32_t K)
{
    uint32_t rank = 0;
    for(uint32_t i = 0; i < K; ++i)
        rank |= base_rank[str[i]] << 2 * (K - i - 1);
    return rank;
}

// Increment the input string to be the next sequence in lexicographic order
void lexicographic_next(std::string& str)
{
    int carry = 1;
    int i = str.size() - 1;
    do {
        uint32_t r = base_rank[str[i]] + carry;
        str[i] = "ACGT"[r % 4];
        carry = r / 4;
        i -= 1;
    } while(carry > 0);
}

// From SO: http://stackoverflow.com/questions/10847007/using-the-gaussian-probability-density-function-in-c
// TODO: replace with a lookup table that can be interpolated
inline double normal_pdf(double x, double m, double s)
{
    static const float inv_sqrt_2pi = 0.3989422804014327;
    double a = (x - m) / s;
    return inv_sqrt_2pi / s * exp(-0.5f * a * a);
}

inline double log_normal_pdf(double x, double m, double s)
{
    static const double log_inv_sqrt_2pi = log(0.3989422804014327);
    double a = (x - m) / s;
    return log_inv_sqrt_2pi - log(s) + (-0.5f * a * a);
}

inline double log_probability_match(const CSquiggleRead& read, 
                                    const char* str,
                                    uint32_t event_idx, 
                                    uint8_t strand)
{
    uint32_t rank = kmer_rank(str, K);
    
    const CPoreModel& pm = read.pore_model[strand];

    // Extract event
    double level = read.events[strand].level[event_idx];
    
    double m = (pm.state[rank].mean + pm.shift) * pm.scale;
    double s = pm.state[rank].sd * pm.scale;
    double lp = log_normal_pdf(level, m, s);

#if DEBUG_HMM_EMISSION
    std::string kmer(str, K);
    printf("Event[%d] Kmer: %s[%d] -- L:%.1lf m: %.1lf s: %.1lf p: %.3lf p_old: %.3lf\n", event_idx, kmer.c_str(), rank, level, m, s, exp(lp), normal_pdf(level, m, s));
#endif

    return lp;
}

void print_matrix(const HMMCell* matrix, uint32_t n_rows, uint32_t n_cols)
{
    for(uint32_t i = 0; i < n_rows; ++i) {
        for(uint32_t j = 0; j < n_cols; ++j) {
            uint32_t c = cell(i, j, n_cols);
            printf("%.2lf\t", matrix[c].M);
        }
        printf("\n");
    }
}

void run_extension_hmm(const std::string& consensus, const HMMConsReadState& state)
{
    double time_start = clock();
    const CSquiggleRead& read = g_data.reads[state.read_idx];

    // TODO: not constant
    double LOG_KMER_INSERTION = log(0.1);

    // The root of the extension sequences to test is the last k-mer
    std::string root_kmer = consensus.substr(consensus.size() - K);
    std::string extension = root_kmer + "AAAAA";
    
    // Get the start/end event indices
    uint32_t e_start = state.event_idx;
    uint32_t e_end = e_start + 10;

    uint32_t k_start = 0;
    uint32_t n_kmers = extension.size() - K + 1;
 
    // Setup transition matrix
    static const uint32_t n_states = 3;
    double t[n_states][n_states] = { { 0.90f, 0.05f, 0.05f },   // MM, ME, MK
                                     { 0.85f, 0.10f, 0.05f },   // EM, EE, EK
                                     { 0.85f, 0.05f, 0.10f } }; // EM, EE, EK

    // Log scale the transition matrix
    for(uint32_t i = 0; i < n_states; ++i) {
        for(uint32_t j = 0; j < n_states; ++j) {
            t[i][j] = log(t[i][j]);
        }
    }
    
    // Set up HMM matrix
    uint32_t n_cols = n_kmers + 1;
    uint32_t n_rows = e_end - e_start + 2;
    uint32_t N = n_rows * n_cols;
    HMMCell* matrix = (HMMCell*)malloc(N * sizeof(HMMCell));
    
    double sum_all_extensions = -INFINITY;
    double best_extension = -INFINITY;
    
    ExtensionResult result;
    for(uint8_t i = 0; i < 4; ++i)
        result.next_kmer[i] = -INFINITY;

    uint32_t extension_rank = 0;
    while(extension.substr(0, K) == root_kmer) {
        
        //
        uint32_t c = cell(0, 0, n_cols);
        matrix[c].M = log(1.0);
        matrix[c].E = -INFINITY;
        matrix[c].K = -INFINITY;

        // Initialize first row/column to prevent initial gaps
        for(uint32_t i = 1; i < n_rows; i++) {
            uint32_t c = cell(i, 0, n_cols);
            matrix[c].M = -INFINITY;
            matrix[c].E = -INFINITY;
            matrix[c].K = -INFINITY;
        }

        for(uint32_t j = 1; j < n_cols; j++) {
            uint32_t c = cell(0, j, n_cols);
            matrix[c].M = -INFINITY;
            matrix[c].E = -INFINITY;
            matrix[c].K = -INFINITY;
        }
        
        // Fill in matrix
        for(uint32_t row = 1; row < n_rows; row++) {
            for(uint32_t col = 1; col < n_cols; col++) {
     
                // cell indices
                uint32_t c = cell(row, col, n_cols);
                uint32_t diag = cell(row - 1, col - 1, n_cols);
                uint32_t up =   cell(row - 1, col, n_cols);
                uint32_t left = cell(row, col - 1, n_cols);

                uint32_t event_idx = e_start + row - 1;
                uint32_t kmer_idx = col - 1;

                // Emission probability for a match
                double l_p_m = log_probability_match(read, extension.c_str() + kmer_idx, event_idx, state.strand);

                // Emission probility for an event insertion
                // This is calculated using the emission probability for a match to the same kmer as the previous row
                double l_p_e = l_p_m;

                // Emission probability for a kmer insertion
                double l_p_k = LOG_KMER_INSERTION;

                // Calculate M[i, j]
                double d_m = t[0][0] + matrix[diag].M;
                double d_e = t[1][0] + matrix[diag].E;
                double d_k = t[2][0] + matrix[diag].K;
                matrix[c].M = l_p_m + log(exp(d_m) + exp(d_e) + exp(d_k));

                // Calculate E[i, j]
                double u_m = t[0][1] + matrix[up].M;
                double u_e = t[1][1] + matrix[up].E;
                double u_k = t[2][1] + matrix[up].K;
                matrix[c].E = l_p_e + log(exp(u_m) + exp(u_e) + exp(u_k));

                // Calculate K[i, j]
                double l_m = t[0][2] + matrix[left].M;
                double l_e = t[1][2] + matrix[left].E;
                double l_k = t[2][2] + matrix[left].K;
                matrix[c].K = l_p_k + log(exp(l_m) + exp(l_e) + exp(l_k));

#ifdef DEBUG_HMM_UPDATE
                printf("(%d %d) R -- [%.2lf %.2lf %.2lf]\n", row, col, matrix[c].M, matrix[c].E, matrix[c].K);
                printf("(%d %d) D -- e: %.2lf t: [%.2lf %.2lf %.2lf] [%.2lf %.2lf %.2lf]\n", row, col, l_p_m, t[0][0], t[1][0], t[2][0], d_m, d_e, d_k);
                printf("(%d %d) U -- e: %.2lf t: [%.2lf %.2lf %.2lf] [%.2lf %.2lf %.2lf]\n", row, col, l_p_e, t[0][1], t[1][1], t[2][1], u_m, u_e, u_k);
                printf("(%d %d) L -- e: %.2lf t: [%.2lf %.2lf %.2lf] [%.2lf %.2lf %.2lf]\n", row, col, l_p_k, t[0][2], t[1][2], t[2][2], l_m, l_e, l_k);
#endif
            }
        }

        // Determine the best scoring row in the last column
        uint32_t col = n_cols - 1;
        uint32_t max_row = 0;
        double max_value = -INFINITY;
        
        for(uint32_t row = 2; row < n_rows; ++row) {
            uint32_t c = cell(row, col, n_cols);
            double sum = log(exp(matrix[c].M) + exp(matrix[c].E) + exp(matrix[c].K));
            if(sum > max_value) {
                max_value = sum;
                max_row = row;
            }
        }

        //print_matrix(matrix, n_rows, n_cols);
        sum_all_extensions = log(exp(sum_all_extensions) + exp(max_value));
        if(max_value > best_extension)
            best_extension = max_value;
        
        /*
        printf("extensions: %s %d %.2lf %.2lf %.2lf\n", 
                extension.substr(extension.size() - K).c_str(), max_row, 
                max_value, best_extension, sum_all_extensions);
        */

        // Store the result for the full k-mer and also summing over all paths
        // starting with a particular base
        result.full_path[extension_rank++] = max_value;
        
        // path sum
        uint8_t br = base_rank[extension[extension.size() - K]];

        double kmer_sum = log(exp(result.next_kmer[br]) + exp(max_value));
        result.next_kmer[br] = kmer_sum;
        
        // Set the extension to the next string
        lexicographic_next(extension);
    }

    for(uint32_t i = 0; i < 4; ++i) {
        printf("L(%c) = %.2lf\n", "ACGT"[i], result.next_kmer[i]);
    }

    double time_stop = clock();
    printf("Time: %.2lfs\n", (time_stop - time_start) / CLOCKS_PER_SEC);
    free(matrix);
}

extern "C"
void run_consensus()
{
    std::string consensus = "AACAG";
    for(uint32_t i = 0; i < g_data.read_states.size(); ++i) {
        run_extension_hmm(consensus, g_data.read_states[i]);
    }
}

int main(int argc, char** argv)
{

}