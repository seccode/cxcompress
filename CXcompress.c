#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <omp.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// Use khash instead of uthash
#define KHASH_INLINE
#include "khash.h"
KHASH_MAP_INIT_STR(strmap, char*)

#define MAX_LINE 1024
#define MAX_ENTRIES 100000

bool symbol_lookup[256][256][256] = {{{ false }}};

typedef struct {
    size_t start;
    size_t len;
    bool is_space;
} TokenSpan;

// Arena allocator for reduced malloc overhead
typedef struct {
    char* buffer;
    size_t size;
    size_t used;
} Arena;

Arena create_arena(size_t size) {
    Arena a = { .buffer = malloc(size), .size = size, .used = 0 };
    if (!a.buffer) { perror("malloc"); exit(1); }
    return a;
}

void* arena_alloc(Arena* a, size_t size) {
    if (a->used + size > a->size) {
        a->size = (a->used + size) * 2;
        a->buffer = realloc(a->buffer, a->size);
        if (!a->buffer) { perror("realloc"); exit(1); }
    }
    void* ptr = a->buffer + a->used;
    a->used += size;
    return ptr;
}

void destroy_arena(Arena* a) {
    free(a->buffer);
    a->buffer = NULL;
    a->size = a->used = 0;
}

// Memory-mapped file I/O
typedef struct {
    char* data;
    size_t size;
} MappedFile;

MappedFile map_file(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) { perror("open"); exit(1); }

    struct stat sb;
    if (fstat(fd, &sb) == -1) { perror("fstat"); exit(1); }

    char* addr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) { perror("mmap"); exit(1); }

    close(fd);
    return (MappedFile){ .data = addr, .size = sb.st_size };
}

void unmap_file(MappedFile mf) {
    munmap(mf.data, mf.size);
}

// Optimized tokenizer
TokenSpan* tokenize(const char* input, size_t len, size_t* token_count, Arena* arena) {
    // Estimate max tokens (every char could be a token in worst case)
    TokenSpan* tokens = arena_alloc(arena, sizeof(TokenSpan) * (len / 2 + 1));
    size_t count = 0;

    const char* p = input;
    const char* end = input + len;
    const char* delims = " ,.?!\n\r";

    while (p < end) {
        const char* start = p;
        if (strchr(delims, *p)) {
            tokens[count++] = (TokenSpan){ .start = start - input, .len = 1, .is_space = true };
            p++;
        } else {
            while (p < end && !strchr(delims, *p)) p++;
            tokens[count++] = (TokenSpan){ .start = start - input, .len = p - start, .is_space = false };
        }
    }

    *token_count = count;
    return tokens;
}

bool is_symbol_fast(const char* word, size_t len) {
    if (len == 0 || len > 3) return false;
    unsigned char a = word[0];
    unsigned char b = (len > 1) ? word[1] : 0;
    unsigned char c = (len > 2) ? word[2] : 0;
    return symbol_lookup[a][b][c];
}

// Thread data structure for parallel processing
typedef struct {
    const char* input;
    const TokenSpan* tokens;
    size_t token_start;
    size_t token_end;
    khash_t(strmap)* hashmap;
    char escape_char;
    char* output;
    size_t output_len;
} ThreadData;

void compress_thread(ThreadData* data) {
    size_t out_pos = 0;
    char temp[256]; // Stack-allocated for small strings

    for (size_t i = data->token_start; i < data->token_end; i++) {
        const TokenSpan tok = data->tokens[i];
        const char* ptr = data->input + tok.start;

        if (tok.is_space) {
            memcpy(data->output + out_pos, ptr, tok.len);
            out_pos += tok.len;
        } else {
            size_t len = tok.len > 255 ? 255 : tok.len;
            memcpy(temp, ptr, len);
            temp[len] = '\0';

            khiter_t k = kh_get(strmap, data->hashmap, temp);
            bool needs_escape = is_symbol_fast(temp, len);

            if (k != kh_end(data->hashmap)) {
                const char* val = kh_value(data->hashmap, k);
                size_t slen = strlen(val);
                memcpy(data->output + out_pos, val, slen);
                out_pos += slen;
            } else if (needs_escape) {
                data->output[out_pos++] = data->escape_char;
                memcpy(data->output + out_pos, ptr, tok.len);
                out_pos += tok.len;
            } else {
                memcpy(data->output + out_pos, ptr, tok.len);
                out_pos += tok.len;
            }
        }
    }

    data->output_len = out_pos;
}

void load_dictionary(const char* dict_path, const char* lang_path,
                    khash_t(strmap)* hashmap, char mode) {
    MappedFile dict_file = map_file(dict_path);
    MappedFile lang_file = map_file(lang_path);

    char* dict_ptr = dict_file.data;
    char* lang_ptr = lang_file.data;
    char* dict_end = dict_file.data + dict_file.size;
    char* lang_end = lang_file.data + lang_file.size;

    while (dict_ptr < dict_end && lang_ptr < lang_end) {
        // Find line endings
        char* dict_line_end = memchr(dict_ptr, '\n', dict_end - dict_ptr);
        char* lang_line_end = memchr(lang_ptr, '\n', lang_end - lang_ptr);
        if (!dict_line_end || !lang_line_end) break;

        *dict_line_end = '\0';
        *lang_line_end = '\0';

        int ret;
        khiter_t k;
        if (mode == 'c') {
            k = kh_put(strmap, hashmap, dict_ptr, &ret);
            if (ret) kh_value(hashmap, k) = strdup(lang_ptr);

            size_t slen = lang_line_end - lang_ptr;
            if (slen > 0 && slen <= 3) { // Ensure slen is positive
                unsigned char a = (unsigned char)lang_ptr[0];
                unsigned char b = (slen > 1) ? (unsigned char)lang_ptr[1] : 0;
                unsigned char c = (slen > 2) ? (unsigned char)lang_ptr[2] : 0;
                symbol_lookup[a][b][c] = true;
            }
        } else {
            k = kh_put(strmap, hashmap, lang_ptr, &ret);
            if (ret) kh_value(hashmap, k) = strdup(dict_ptr);
        }

        dict_ptr = dict_line_end + 1;
        lang_ptr = lang_line_end + 1;
    }

    unmap_file(dict_file);
    unmap_file(lang_file);
}

char find_unused_char(const char* data, size_t len) {
    bool used[256] = {0};
    used[0] = true; // Null character
    for (size_t i = 0; i < len; i++) used[(unsigned char)data[i]] = true;
    for (int i = 1; i < 256; i++) if (!used[i]) return (char)i;
    fprintf(stderr, "No escape character available\n"); exit(1);
}

void compress(const char* dict_path, const char* lang_path,
              const char* input, size_t input_len, int threads) {
    // Initialize hash table and load dictionary
    khash_t(strmap)* hashmap = kh_init(strmap);
    load_dictionary(dict_path, lang_path, hashmap, 'c');
    char escape_char = find_unused_char(input, input_len);

    // Tokenize input
    Arena arena = create_arena(input_len * 2);
    size_t token_count;
    TokenSpan* tokens = tokenize(input, input_len, &token_count, &arena);

    // Prepare output file
    FILE* out = fopen("out.compressed", "wb");
    if (!out) { perror("fopen"); exit(1); }
    fputc(escape_char, out);

    // Parallel compression
    ThreadData* thread_data = malloc(sizeof(ThreadData) * threads);
    char** outputs = malloc(sizeof(char*) * threads);
    size_t tokens_per_thread = (token_count + threads - 1) / threads;

    #pragma omp parallel num_threads(threads)
    {
        int tid = omp_get_thread_num();
        size_t start = tid * tokens_per_thread;
        size_t end = (tid + 1) * tokens_per_thread;
        if (end > token_count) end = token_count;

        outputs[tid] = malloc(input_len * 2); // Worst case expansion

        ThreadData data = {
            .input = input,
            .tokens = tokens,
            .token_start = start,
            .token_end = end,
            .hashmap = hashmap,
            .escape_char = escape_char,
            .output = outputs[tid]
        };

        compress_thread(&data);
        thread_data[tid] = data;
    }

    // Write output sequentially
    for (int i = 0; i < threads; i++) {
        fwrite(thread_data[i].output, 1, thread_data[i].output_len, out);
        free(outputs[i]);
    }

    // Cleanup
    fclose(out);
    free(outputs);
    free(thread_data);

    // Free hash table values
    khiter_t k;
    for (k = kh_begin(hashmap); k != kh_end(hashmap); ++k) {
        if (kh_exist(hashmap, k)) {
            free(kh_value(hashmap, k));
        }
    }
    kh_destroy(strmap, hashmap);
    destroy_arena(&arena);
}

void decompress(const char* dict_path, const char* lang_path,
                const char* input, size_t input_len, int threads) {
    if (input_len < 1) return;
    char escape_char = input[0];
    const char* data_input = input + 1;
    size_t data_len = input_len - 1;

    // Initialize hash table and load dictionary
    khash_t(strmap)* hashmap = kh_init(strmap);
    load_dictionary(lang_path, dict_path, hashmap, 'd');

    // Tokenize input
    Arena arena = create_arena(data_len * 2);
    size_t token_count;
    TokenSpan* tokens = tokenize(data_input, data_len, &token_count, &arena);

    // Prepare output file
    FILE* out = fopen("out.decompressed", "wb");
    if (!out) { perror("fopen"); exit(1); }

    // Parallel decompression
    ThreadData* thread_data = malloc(sizeof(ThreadData) * threads);
    char** outputs = malloc(sizeof(char*) * threads);
    size_t tokens_per_thread = (token_count + threads - 1) / threads;

    #pragma omp parallel num_threads(threads)
    {
        int tid = omp_get_thread_num();
        size_t start = tid * tokens_per_thread;
        size_t end = (tid + 1) * tokens_per_thread;
        if (end > token_count) end = token_count;

        outputs[tid] = malloc(data_len * 2); // Worst case expansion

        ThreadData data = {
            .input = data_input, // Use data_input
            .tokens = tokens,
            .token_start = start,
            .token_end = end,
            .hashmap = hashmap,
            .escape_char = escape_char,
            .output = outputs[tid]
        };

        compress_thread(&data); // Reuse same processing function
        thread_data[tid] = data;
    }

    // Write output sequentially
    for (int i = 0; i < threads; i++) {
        fwrite(thread_data[i].output, 1, thread_data[i].output_len, out);
        free(outputs[i]);
    }

    // Cleanup
    fclose(out);
    free(outputs);
    free(thread_data);

    // Free hash table values
    khiter_t k;
    for (k = kh_begin(hashmap); k != kh_end(hashmap); ++k) {
        if (kh_exist(hashmap, k)) {
            free(kh_value(hashmap, k));
        }
    }
    kh_destroy(strmap, hashmap);
    destroy_arena(&arena);
}

int main(int argc, char* argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <-c|-d> <file> <dict> <lang> <threads>\n", argv[0]);
        return 1;
    }

    const char* mode = argv[1];
    const char* file_path = argv[2];
    const char* dict_path = argv[3];
    const char* lang_path = argv[4];
    int threads = atoi(argv[5]);

    MappedFile input = map_file(file_path);

    if (strcmp(mode, "-c") == 0) {
        compress(dict_path, lang_path, input.data, input.size, threads);
    } else if (strcmp(mode, "-d") == 0) {
        decompress(dict_path, lang_path, input.data, input.size, threads);
    } else {
        fprintf(stderr, "Invalid mode: use -c (compress) or -d (decompress)\n");
        unmap_file(input);
        return 1;
    }

    unmap_file(input);
    return 0;
}
