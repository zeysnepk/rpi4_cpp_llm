#include "llama.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Qwen2.5 chat template
static std::string format_qwen_prompt(const std::string& user_msg) {
    return "<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n"
           "<|im_start|>user\n" + user_msg + "<|im_end|>\n"
           "<|im_start|>assistant\n";
}

int main(int argc, char ** argv) {
    if (argc < 3) {
        fprintf(stderr, "Kullanim: %s <model.gguf> \"<prompt>\" [n_predict]\n", argv[0]);
        return 1;
    }

    const char * model_path     = argv[1];
    const std::string user_msg  = argv[2];
    const int n_predict         = (argc > 3) ? std::atoi(argv[3]) : 256;

    // Qwen chat template uygula
    const std::string prompt = format_qwen_prompt(user_msg);

    // RPi4 icin optimize parametreler
    const int n_threads = 4;
    const int n_ctx     = 2048;
    const int n_batch   = 256;

    // 1) Backend yukle
    ggml_backend_load_all();
    llama_log_set([](ggml_log_level, const char*, void*){}, nullptr); // gurultuyu kis

    // 2) Modeli yukle (mlock + mmap)
    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;
    mparams.use_mlock    = true;
    mparams.use_mmap     = true;

    fprintf(stderr, "Model yukleniyor (mlock ile RAM'e kilitleniyor)...\n");
    llama_model * model = llama_model_load_from_file(model_path, mparams);
    if (!model) {
        fprintf(stderr, "HATA: Model yuklenemedi: %s\n", model_path);
        return 1;
    }
    const llama_vocab * vocab = llama_model_get_vocab(model);

    // 3) Context olustur
    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx           = n_ctx;
    cparams.n_batch         = n_batch;
    cparams.n_threads       = n_threads;
    cparams.n_threads_batch = n_threads;

    llama_context * ctx = llama_init_from_model(model, cparams);
    if (!ctx) {
        fprintf(stderr, "HATA: Context olusturulamadi.\n");
        llama_model_free(model);
        return 1;
    }

    // 4) Sampler zinciri
    auto sparams = llama_sampler_chain_default_params();
    llama_sampler * smpl = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.95f, 1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.7f));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    // 5) Tokenize
    const int n_prompt = -llama_tokenize(vocab, prompt.c_str(), prompt.size(),
                                         nullptr, 0, true, true);
    std::vector<llama_token> tokens(n_prompt);
    if (llama_tokenize(vocab, prompt.c_str(), prompt.size(),
                       tokens.data(), tokens.size(), true, true) < 0) {
        fprintf(stderr, "HATA: Tokenize basarisiz.\n");
        return 1;
    }
    fprintf(stderr, "Prompt token sayisi: %d\n--- Yanit ---\n", n_prompt);

    // 6) Prefill
    llama_batch batch = llama_batch_get_one(tokens.data(), tokens.size());
    if (llama_decode(ctx, batch) != 0) {
        fprintf(stderr, "HATA: Prefill basarisiz.\n");
        return 1;
    }

    // 7) Token uretim dongusu
    const int64_t t_start = ggml_time_us();
    int n_generated = 0;

    for (int i = 0; i < n_predict; i++) {
        llama_token new_token = llama_sampler_sample(smpl, ctx, -1);
        if (llama_vocab_is_eog(vocab, new_token)) break;

        char buf[256];
        int n = llama_token_to_piece(vocab, new_token, buf, sizeof(buf), 0, true);
        if (n < 0) break;
        fwrite(buf, 1, n, stdout);
        fflush(stdout);

        batch = llama_batch_get_one(&new_token, 1);
        if (llama_decode(ctx, batch) != 0) break;
        n_generated++;
    }

    const double t_sec = (ggml_time_us() - t_start) / 1e6;
    fprintf(stderr, "\n\n--- Istatistik ---\n");
    fprintf(stderr, "Token: %d | Sure: %.2fs | Hiz: %.2f tok/s\n",
            n_generated, t_sec, n_generated / t_sec);

    // 8) Temizlik
    llama_sampler_free(smpl);
    llama_free(ctx);
    llama_model_free(model);
    return 0;
}