#include "ggml_v2.h"
#include "otherarch.h"

#include "utils.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <iostream>



// load the model's weights from a file
ModelLoadResult gpt_neox_v2_model_load(const std::string & fname, gpt_neox_v2_model & model, gpt_vocab & vocab, FileFormat file_format) {
    printf("%s: loading model from '%s' - please wait ...\n", __func__, fname.c_str());

    auto fin = std::ifstream(fname, std::ios::binary);
    if (!fin) {
        fprintf(stderr, "%s: failed to open '%s'\n", __func__, fname.c_str());
        return ModelLoadResult::FAIL;
    }

    // verify magic
    {
        uint32_t magic;
        fin.read((char *) &magic, sizeof(magic));
        if (magic != 0x67676d6c) {
            fprintf(stderr, "%s: invalid model file '%s' (bad magic)\n", __func__, fname.c_str());
            return ModelLoadResult::FAIL;
        }
    }

    // load hparams
    {
        auto & hparams = model.hparams;
        hparams.par_res = 1; //true
        fin.read((char *) &hparams.n_vocab, sizeof(hparams.n_vocab));
        fin.read((char *) &hparams.n_ctx,   sizeof(hparams.n_ctx));
        fin.read((char *) &hparams.n_embd,  sizeof(hparams.n_embd));
        fin.read((char *) &hparams.n_head,  sizeof(hparams.n_head));
        fin.read((char *) &hparams.n_layer, sizeof(hparams.n_layer));
        fin.read((char *) &hparams.n_rot,   sizeof(hparams.n_rot));
        if(file_format!=FileFormat::NEOX_1 && file_format!=FileFormat::NEOX_2 && file_format!=FileFormat::NEOX_3)
        {
            fin.read((char *) &hparams.par_res, sizeof(hparams.par_res));
        }
        if(file_format==FileFormat::NEOX_3)
        {
            hparams.par_res = 0;
        }
        fin.read((char *) &hparams.ftype,   sizeof(hparams.ftype));

        const int32_t qntvr = hparams.ftype / GGML_V2_QNT_VERSION_FACTOR;

        printf("%s: n_vocab = %d\n", __func__, hparams.n_vocab);
        printf("%s: n_ctx   = %d\n", __func__, hparams.n_ctx);
        printf("%s: n_embd  = %d\n", __func__, hparams.n_embd);
        printf("%s: n_head  = %d\n", __func__, hparams.n_head);
        printf("%s: n_layer = %d\n", __func__, hparams.n_layer);
        printf("%s: n_rot   = %d\n", __func__, hparams.n_rot);
        printf("%s: par_res = %d\n", __func__, hparams.par_res);
        printf("%s: ftype   = %d\n", __func__, hparams.ftype);
        printf("%s: qntvr   = %d\n", __func__, qntvr);

        hparams.ftype %= GGML_V2_QNT_VERSION_FACTOR;
    }

    // load vocab
    {
        const int32_t n_vocab = model.hparams.n_vocab;

        std::string word;
        std::vector<char> buf(128);

        for (int i = 0; i < n_vocab; i++) {
            uint32_t len;
            fin.read((char *) &len, sizeof(len));

            buf.resize(len);
            fin.read((char *) buf.data(), len);
            word.assign(buf.data(), len);

            vocab.token_to_id[word] = i;
            vocab.id_to_token[i] = word;
        }
    }

    // for the big tensors, we have the option to store the data in 16-bit floats or quantized
    // in order to save memory and also to speed up the computation
    ggml_v2_type wtype = ggml_v2_ftype_to_ggml_v2_type((ggml_v2_ftype) (model.hparams.ftype));
    if (wtype == GGML_V2_TYPE_COUNT) {
        fprintf(stderr, "%s: invalid model file '%s' (bad ftype value %d)\n",
                __func__, fname.c_str(), model.hparams.ftype);
        return ModelLoadResult::FAIL;
    }

    auto & ctx = model.ctx;

    size_t ctx_size = 0;

    {
        const auto & hparams = model.hparams;

        const int n_embd  = hparams.n_embd;
        const int n_layer = hparams.n_layer;
        const int n_ctx   = hparams.n_ctx;
        const int n_vocab = hparams.n_vocab;

        ctx_size += n_embd*ggml_v2_type_sizef(GGML_V2_TYPE_F32); // ln_f_g
        ctx_size += n_embd*ggml_v2_type_sizef(GGML_V2_TYPE_F32); // ln_f_b

        ctx_size += n_embd*n_vocab*ggml_v2_type_sizef(wtype); // wte

        ctx_size += n_embd*n_vocab*ggml_v2_type_sizef(wtype);           // lmh_g
        //ctx_size +=        n_vocab*ggml_v2_type_sizef(GGML_V2_TYPE_F32); // lmh_b

        ctx_size += n_layer*(n_embd*ggml_v2_type_sizef(GGML_V2_TYPE_F32)); // ln_1_g
        ctx_size += n_layer*(n_embd*ggml_v2_type_sizef(GGML_V2_TYPE_F32)); // ln_1_b

        ctx_size += n_layer*(3*n_embd*n_embd*ggml_v2_type_sizef(wtype));         // c_attn_attn_w
        ctx_size += n_layer*(       3*n_embd*ggml_v2_type_sizef(GGML_V2_TYPE_F32)); // c_attn_attn_b

        ctx_size += n_layer*(n_embd*n_embd*ggml_v2_type_sizef(wtype));         // c_attn_proj_w
        ctx_size += n_layer*(n_embd*n_embd*ggml_v2_type_sizef(GGML_V2_TYPE_F32)); // c_attn_proj_b

        ctx_size += n_layer*(n_embd*ggml_v2_type_sizef(GGML_V2_TYPE_F32)); // ln_2_g
        ctx_size += n_layer*(n_embd*ggml_v2_type_sizef(GGML_V2_TYPE_F32)); // ln_2_b

        ctx_size += n_layer*(4*n_embd*n_embd*ggml_v2_type_sizef(wtype));         // c_mlp_fc_w
        ctx_size += n_layer*(       4*n_embd*ggml_v2_type_sizef(GGML_V2_TYPE_F32)); // c_mlp_fc_b

        ctx_size += n_layer*(4*n_embd*n_embd*ggml_v2_type_sizef(wtype));         // c_mlp_proj_w
        ctx_size += n_layer*(         n_embd*ggml_v2_type_sizef(GGML_V2_TYPE_F32)); // c_mlp_proj_b

        ctx_size += n_ctx*n_layer*n_embd*ggml_v2_type_sizef(GGML_V2_TYPE_F32); // memory_k
        ctx_size += n_ctx*n_layer*n_embd*ggml_v2_type_sizef(GGML_V2_TYPE_F32); // memory_v

        ctx_size += (6 + 16*n_layer)*512; // object overhead

        printf("%s: ggml ctx size = %6.2f MB\n", __func__, ctx_size/(1024.0*1024.0));
    }

    // create the ggml context
    {
        struct ggml_v2_init_params params;
        params.mem_size   = ctx_size;
        params.mem_buffer = NULL;
        params.no_alloc   = false;
        
        model.ctx = ggml_v2_init(params);
        if (!model.ctx) {
            fprintf(stderr, "%s: ggml_v2_init() failed\n", __func__);
            return ModelLoadResult::FAIL;
        }
    }

    // prepare memory for the weights
    {
        const auto & hparams = model.hparams;

        const int n_embd  = hparams.n_embd;
        const int n_layer = hparams.n_layer;
        const int n_vocab = hparams.n_vocab;

        model.layers.resize(n_layer);

        model.wte    = ggml_v2_new_tensor_2d(ctx, wtype,         n_embd, n_vocab);

        model.ln_f_g = ggml_v2_new_tensor_1d(ctx, GGML_V2_TYPE_F32, n_embd);
        model.ln_f_b = ggml_v2_new_tensor_1d(ctx, GGML_V2_TYPE_F32, n_embd);

        model.lmh_g  = ggml_v2_new_tensor_2d(ctx, wtype,         n_embd, n_vocab);
        //model.lmh_b  = ggml_v2_new_tensor_1d(ctx, GGML_V2_TYPE_F32, n_vocab);

        // map by name
        model.tensors["gpt_neox.embed_in.weight"] = model.wte;

        model.tensors["gpt_neox.final_layer_norm.weight"] = model.ln_f_g;
        model.tensors["gpt_neox.final_layer_norm.bias"]   = model.ln_f_b;

        model.tensors["embed_out.weight"] = model.lmh_g;
        //model.tensors["lm_head.bias"]   = model.lmh_b;

        for (int i = 0; i < n_layer; ++i) {
            auto & layer = model.layers[i];

            layer.ln_1_g          = ggml_v2_new_tensor_1d(ctx, GGML_V2_TYPE_F32,   n_embd);
            layer.ln_1_b          = ggml_v2_new_tensor_1d(ctx, GGML_V2_TYPE_F32,   n_embd);

            layer.c_attn_attn_w   = ggml_v2_new_tensor_2d(ctx, wtype,           n_embd, 3*n_embd);
            layer.c_attn_attn_b   = ggml_v2_new_tensor_1d(ctx, GGML_V2_TYPE_F32, 3*n_embd);

            layer.c_attn_proj_w   = ggml_v2_new_tensor_2d(ctx, wtype,           n_embd,   n_embd);
            layer.c_attn_proj_b   = ggml_v2_new_tensor_1d(ctx, GGML_V2_TYPE_F32,   n_embd);

            layer.ln_2_g          = ggml_v2_new_tensor_1d(ctx, GGML_V2_TYPE_F32,   n_embd);
            layer.ln_2_b          = ggml_v2_new_tensor_1d(ctx, GGML_V2_TYPE_F32,   n_embd);

            layer.c_mlp_fc_w      = ggml_v2_new_tensor_2d(ctx, wtype,           n_embd, 4*n_embd);
            layer.c_mlp_fc_b      = ggml_v2_new_tensor_1d(ctx, GGML_V2_TYPE_F32, 4*n_embd);

            layer.c_mlp_proj_w    = ggml_v2_new_tensor_2d(ctx, wtype,         4*n_embd,   n_embd);
            layer.c_mlp_proj_b    = ggml_v2_new_tensor_1d(ctx, GGML_V2_TYPE_F32,   n_embd);

            // map by name
            model.tensors["gpt_neox.layers." + std::to_string(i) + ".input_layernorm.weight"] = layer.ln_1_g;
            model.tensors["gpt_neox.layers." + std::to_string(i) + ".input_layernorm.bias"]   = layer.ln_1_b;

            model.tensors["gpt_neox.layers." + std::to_string(i) + ".attention.query_key_value.weight"] = layer.c_attn_attn_w;
            model.tensors["gpt_neox.layers." + std::to_string(i) + ".attention.query_key_value.bias"]   = layer.c_attn_attn_b;

            model.tensors["gpt_neox.layers." + std::to_string(i) + ".attention.dense.weight"] = layer.c_attn_proj_w;
            model.tensors["gpt_neox.layers." + std::to_string(i) + ".attention.dense.bias"]   = layer.c_attn_proj_b;

            model.tensors["gpt_neox.layers." + std::to_string(i) + ".post_attention_layernorm.weight"] = layer.ln_2_g;
            model.tensors["gpt_neox.layers." + std::to_string(i) + ".post_attention_layernorm.bias"]   = layer.ln_2_b;

            model.tensors["gpt_neox.layers." + std::to_string(i) + ".mlp.dense_h_to_4h.weight"] = layer.c_mlp_fc_w;
            model.tensors["gpt_neox.layers." + std::to_string(i) + ".mlp.dense_h_to_4h.bias"]   = layer.c_mlp_fc_b;

            model.tensors["gpt_neox.layers." + std::to_string(i) + ".mlp.dense_4h_to_h.weight"] = layer.c_mlp_proj_w;
            model.tensors["gpt_neox.layers." + std::to_string(i) + ".mlp.dense_4h_to_h.bias"]   = layer.c_mlp_proj_b;
        }
    }

    // key + value memory
    {
        const auto & hparams = model.hparams;

        const int n_embd  = hparams.n_embd;
        const int n_layer = hparams.n_layer;
        const int n_ctx   = hparams.n_ctx;

        const int64_t n_mem      = n_layer*n_ctx;
        const int64_t n_elements = n_embd*n_mem;

        model.memory_k = ggml_v2_new_tensor_1d(ctx, GGML_V2_TYPE_F16, n_elements);
        model.memory_v = ggml_v2_new_tensor_1d(ctx, GGML_V2_TYPE_F16, n_elements);

        const size_t memory_size = ggml_v2_nbytes(model.memory_k) + ggml_v2_nbytes(model.memory_v);

        printf("%s: memory_size = %8.2f MB, n_mem = %" PRId64 "\n", __func__, memory_size/1024.0/1024.0, n_mem);
    }

    // load weights
    {
        int n_tensors = 0;
        size_t total_size = 0;

        printf("%s: ", __func__);

        while (true) {
            int32_t n_dims;
            int32_t length;
            int32_t ttype;

            fin.read(reinterpret_cast<char *>(&n_dims), sizeof(n_dims));
            fin.read(reinterpret_cast<char *>(&length), sizeof(length));
            fin.read(reinterpret_cast<char *>(&ttype),  sizeof(ttype));

            if (fin.eof()) {
                break;
            }

            int32_t nelements = 1;
            int32_t ne[2] = { 1, 1 };
            for (int i = 0; i < n_dims; ++i) {
                fin.read(reinterpret_cast<char *>(&ne[i]), sizeof(ne[i]));
                nelements *= ne[i];
            }

            std::string name(length, 0);
            fin.read(&name[0], length);

            if (model.tensors.find(name.data()) == model.tensors.end()) {
                fprintf(stderr, "%s: unknown tensor '%s' in model file\n", __func__, name.data());
                return ModelLoadResult::FAIL;
            }

            auto tensor = model.tensors[name.data()];
            if (ggml_v2_nelements(tensor) != nelements) {
                fprintf(stderr, "%s: tensor '%s' has wrong size in model file\n", __func__, name.data());
                return ModelLoadResult::FAIL;
            }

            if (tensor->ne[0] != ne[0] || tensor->ne[1] != ne[1]) {
                fprintf(stderr, "%s: tensor '%s' has wrong shape in model file: got [%5d, %5d], expected [%5d, %5d]\n",
                        __func__, name.data(), (int) tensor->ne[0], (int) tensor->ne[1], ne[0], ne[1]);
                return ModelLoadResult::FAIL;
            }

            // for debugging
            if (0) {
                printf("%24s - [%5d, %5d], type = %6s, %6.2f MB, %9zu bytes\n", name.data(), ne[0], ne[1], ggml_v2_type_name(ggml_v2_type(ttype)), ggml_v2_nbytes(tensor)/1024.0/1024.0, ggml_v2_nbytes(tensor));
            }

            size_t bpe = ggml_v2_type_size(ggml_v2_type(ttype));

            if(file_format==FileFormat::NEOX_1)
            {
                switch (ttype) {
                    case 0: bpe = ggml_v2_type_size(GGML_V2_TYPE_F32);  break;
                    case 1: bpe = ggml_v2_type_size(GGML_V2_TYPE_F16);  break;
                    case 2: bpe = ggml_v2_type_size(GGML_V2_TYPE_Q4_0); assert(ne[0] % 64 == 0); break;
                    case 3: bpe = ggml_v2_type_size(GGML_V2_TYPE_Q4_1); assert(ne[0] % 64 == 0); break;
                    case 5: bpe = ggml_v2_type_size(GGML_V2_TYPE_Q4_2); assert(ne[0] % 64 == 0); break;
                    case 6: bpe = ggml_v2_type_size(GGML_V2_TYPE_Q4_3); assert(ne[0] % 64 == 0); break;
                    default:
                    {
                        fprintf(stderr, "%s: unknown ftype %d in model file\n", __func__, ttype);
                        return ModelLoadResult::FAIL;
                    }
                };
            }

            if ((nelements*bpe)/ggml_v2_blck_size(tensor->type) != ggml_v2_nbytes(tensor)) {
                fprintf(stderr, "%s: tensor '%s' has wrong size in model file: got %zu, expected %zu\n",
                        __func__, name.data(), ggml_v2_nbytes(tensor), nelements*bpe);
                 ggml_v2_free(ctx);
                 return ModelLoadResult::RETRY_LOAD;
            }

            fin.read(reinterpret_cast<char *>(tensor->data), ggml_v2_nbytes(tensor));

            total_size += ggml_v2_nbytes(tensor);
            if (++n_tensors % 8 == 0) {
                printf(".");
                fflush(stdout);
            }
        }

        printf(" done\n");

        printf("%s: model size = %8.2f MB / num tensors = %d\n", __func__, total_size/1024.0/1024.0, n_tensors);
    }

    fin.close();

    return ModelLoadResult::SUCCESS;
}


// feed-forward network
ggml_v2_tensor * gpt_neox_ff(
        const gpt_neox_layer_v2 &layer,
        ggml_v2_context * ctx0,
        ggml_v2_tensor * inp) {
    ggml_v2_tensor * cur = ggml_v2_norm(ctx0, inp);

    cur = ggml_v2_add(ctx0,
        ggml_v2_mul(ctx0,
            ggml_v2_repeat(ctx0, layer.ln_2_g, cur),
            cur),
        ggml_v2_repeat(ctx0, layer.ln_2_b, cur));

    cur = ggml_v2_mul_mat(ctx0,
            layer.c_mlp_fc_w,
            cur);

    cur = ggml_v2_add(ctx0,
            ggml_v2_repeat(ctx0, layer.c_mlp_fc_b, cur),
            cur);

    // GELU activation
    cur = ggml_v2_gelu(ctx0, cur);

    // projection
    // cur = proj_w*cur + proj_b
    cur = ggml_v2_mul_mat(ctx0,
            layer.c_mlp_proj_w,
            cur);

    cur = ggml_v2_add(ctx0,
            ggml_v2_repeat(ctx0, layer.c_mlp_proj_b, cur),
            cur);
    return cur;
}

// evaluate the transformer
//
//   - model:     the model
//   - n_threads: number of threads to use
//   - n_past:    the context size so far
//   - embd_inp:  the embeddings of the tokens in the context
//   - embd_w:    the predicted logits for the next token
//
bool gpt_neox_v2_eval(
        const gpt_neox_v2_model & model,
        const int n_threads,
        const int n_past,
        const std::vector<gpt_vocab::id> & embd_inp,
              std::vector<float>         & embd_w,
              size_t                     & mem_per_token) {
    const int N = embd_inp.size();

    const auto & hparams = model.hparams;

    const int n_embd  = hparams.n_embd;
    const int n_layer = hparams.n_layer;
    const int n_ctx   = hparams.n_ctx;
    const int n_head  = hparams.n_head;
    const int n_vocab = hparams.n_vocab;
    const int n_rot   = hparams.n_rot;

    static size_t buf_size = 256u*1024*1024;
    static void * buf = malloc(buf_size);

    if (mem_per_token > 0 && (mem_per_token*N*2 + 64u*1024*1024) > buf_size) {
        const size_t buf_size_new = 360u*1024*1024 + 2*(mem_per_token*N); // add 10% to account for ggml object overhead
        //printf("\n%s: reallocating buffer from %zu to %zu bytes\n", __func__, buf_size, buf_size_new);

        // reallocate
        buf_size = buf_size_new;
        buf = realloc(buf, buf_size);
        if (buf == nullptr) {
            fprintf(stderr, "%s: failed to allocate %zu bytes\n", __func__, buf_size);
            return false;
        }
    }

    struct ggml_v2_init_params params;
    params.mem_size   = buf_size;
    params.mem_buffer = buf;
    params.no_alloc   = false;
    

    struct ggml_v2_context * ctx0 = ggml_v2_init(params);
    struct ggml_v2_cgraph gf = {};
    gf.n_threads = n_threads;

    struct ggml_v2_tensor * embd = ggml_v2_new_tensor_1d(ctx0, GGML_V2_TYPE_I32, N);
    memcpy(embd->data, embd_inp.data(), N*ggml_v2_element_size(embd));

    // wte
    struct ggml_v2_tensor * inpL = ggml_v2_get_rows(ctx0, model.wte, embd);

    for (int il = 0; il < n_layer; ++il) {
        struct ggml_v2_tensor * cur;

        // self-attention
        {
            {
                cur = ggml_v2_norm(ctx0, inpL);

                cur = ggml_v2_add(ctx0,
                        ggml_v2_mul(ctx0,
                            ggml_v2_repeat(ctx0, model.layers[il].ln_1_g, cur),
                            cur),
                        ggml_v2_repeat(ctx0, model.layers[il].ln_1_b, cur));
            }

            // compute QKV
            {
                cur = ggml_v2_mul_mat(ctx0,
                        model.layers[il].c_attn_attn_w,
                        cur);

                cur = ggml_v2_add(ctx0,
                        ggml_v2_repeat(ctx0, model.layers[il].c_attn_attn_b, cur),
                        cur);
            }

            struct ggml_v2_tensor * Qcur = ggml_v2_cont(ctx0, ggml_v2_view_3d(ctx0, cur, n_embd/n_head, n_head, N, cur->nb[1]/n_head, cur->nb[1], 0*sizeof(float)*n_embd/n_head));
            struct ggml_v2_tensor * Kcur = ggml_v2_cont(ctx0, ggml_v2_view_3d(ctx0, cur, n_embd/n_head, n_head, N, cur->nb[1]/n_head, cur->nb[1], 1*sizeof(float)*n_embd/n_head));
            struct ggml_v2_tensor * Vcur = ggml_v2_cont(ctx0, ggml_v2_view_3d(ctx0, cur, n_embd/n_head, n_head, N, cur->nb[1]/n_head, cur->nb[1], 2*sizeof(float)*n_embd/n_head));

            // using mode = 2 for GPT-NeoX mode
            Qcur = ggml_v2_rope_inplace(ctx0, Qcur, n_past, n_rot, 2);
            Kcur = ggml_v2_rope_inplace(ctx0, Kcur, n_past, n_rot, 2);

            // store key and value to memory
            {
                Vcur = ggml_v2_transpose(ctx0, ggml_v2_reshape_2d(ctx0, Vcur, n_embd, N));

                struct ggml_v2_tensor * k = ggml_v2_view_1d(ctx0, model.memory_k, N*n_embd, (ggml_v2_element_size(model.memory_k)*n_embd)*(il*n_ctx + n_past));
                struct ggml_v2_tensor * v = ggml_v2_view_2d(ctx0, model.memory_v, N, n_embd,
                        (   n_ctx)*ggml_v2_element_size(model.memory_v),
                        (il*n_ctx)*ggml_v2_element_size(model.memory_v)*n_embd + n_past*ggml_v2_element_size(model.memory_v));

                ggml_v2_build_forward_expand(&gf, ggml_v2_cpy(ctx0, Kcur, k));
                ggml_v2_build_forward_expand(&gf, ggml_v2_cpy(ctx0, Vcur, v));
            }

            // Q = Qcur.contiguous().view(n_embd/n_head, n_head, N).permute(0, 2, 1, 3)
            struct ggml_v2_tensor * Q =
                ggml_v2_permute(ctx0,
                        Qcur,
                        0, 2, 1, 3);

            // K = Kmem.view(n_embd/n_head, n_head, n_past + N).permute(0, 2, 1, 3)
            struct ggml_v2_tensor * K =
                ggml_v2_permute(ctx0,
                        ggml_v2_reshape_3d(ctx0,
                            ggml_v2_view_1d(ctx0, model.memory_k, (n_past + N)*n_embd, il*n_ctx*ggml_v2_element_size(model.memory_k)*n_embd),
                            n_embd/n_head, n_head, n_past + N),
                        0, 2, 1, 3);

            // K * Q
            struct ggml_v2_tensor * KQ = ggml_v2_mul_mat(ctx0, K, Q);

            // KQ_scaled = KQ / sqrt(n_embd/n_head)
            struct ggml_v2_tensor * KQ_scaled =
                ggml_v2_scale_inplace(ctx0,
                        KQ,
                        ggml_v2_new_f32(ctx0, 1.0f/sqrt(float(n_embd)/n_head))
                        );

            // KQ_masked = mask_past(KQ_scaled)
            struct ggml_v2_tensor * KQ_masked = ggml_v2_diag_mask_inf_inplace(ctx0, KQ_scaled, n_past);

            // KQ = soft_max(KQ_masked)
            struct ggml_v2_tensor * KQ_soft_max = ggml_v2_soft_max_inplace(ctx0, KQ_masked);

            // V_trans = Vmem.view(n_embd/n_head, n_head, n_past + N).permute(1, 2, 0, 3).contiguous()
            struct ggml_v2_tensor * V =
                ggml_v2_view_3d(ctx0, model.memory_v,
                        n_past + N, n_embd/n_head, n_head,
                        n_ctx*ggml_v2_element_size(model.memory_v),
                        n_ctx*ggml_v2_element_size(model.memory_v)*n_embd/n_head,
                        il*n_ctx*ggml_v2_element_size(model.memory_v)*n_embd);

            // KQV = transpose(V) * KQ_soft_max
            struct ggml_v2_tensor * KQV = ggml_v2_mul_mat(ctx0, V, KQ_soft_max);

            // KQV_merged = KQV.permute(0, 2, 1, 3)
            struct ggml_v2_tensor * KQV_merged = ggml_v2_permute(ctx0, KQV, 0, 2, 1, 3);

            // cur = KQV_merged.contiguous().view(n_embd, N)
            cur = ggml_v2_cpy(ctx0,
                    KQV_merged,
                    ggml_v2_new_tensor_2d(ctx0, GGML_V2_TYPE_F32, n_embd, N));

            // projection
            {
                cur = ggml_v2_mul_mat(ctx0,
                        model.layers[il].c_attn_proj_w,
                        cur);

                cur = ggml_v2_add(ctx0, ggml_v2_repeat(ctx0, model.layers[il].c_attn_proj_b, cur), cur);
            }
        }

        if (hparams.par_res == 0) {
            struct ggml_v2_tensor * inpFF = ggml_v2_add(ctx0, cur, inpL);

            cur = gpt_neox_ff(model.layers[il], ctx0, inpFF);

            // input for next layer
            inpL = ggml_v2_add(ctx0, cur, inpFF);
        } else {
            struct ggml_v2_tensor * inpFF = cur;

            // this is independent of the self-attention result, so it could be done in parallel to the self-attention
            // note here we pass inpL instead of cur
            cur = gpt_neox_ff(model.layers[il], ctx0, inpL);

            // layer input + FF
            cur  = ggml_v2_add(ctx0, cur, inpFF);

            // input for next layer
            inpL = ggml_v2_add(ctx0, cur, inpL);
        }
    }

    // norm
    {
        inpL = ggml_v2_norm(ctx0, inpL);

        // inpL = ln_f_g*inpL + ln_f_b
        inpL = ggml_v2_add(ctx0,
                ggml_v2_mul(ctx0,
                    ggml_v2_repeat(ctx0, model.ln_f_g, inpL),
                    inpL),
                ggml_v2_repeat(ctx0, model.ln_f_b, inpL));
    }

    // lm_head
    {
        inpL = ggml_v2_mul_mat(ctx0, model.lmh_g, inpL);

        //inpL = ggml_v2_add(ctx0,
        //        ggml_v2_repeat(ctx0, model.lmh_b, inpL),
        //        inpL);
    }

    // logits -> probs
    //inpL = ggml_v2_soft_max_inplace(ctx0, inpL);

    // run the computation
    ggml_v2_build_forward_expand(&gf, inpL);
    ggml_v2_graph_compute       (ctx0, &gf);

    //if (n_past%100 == 0) {
    //    ggml_v2_graph_print   (&gf);
    //    ggml_v2_graph_dump_dot(&gf, NULL, "gpt-2.dot");
    //}

    //embd_w.resize(n_vocab*N);
    //memcpy(embd_w.data(), ggml_v2_get_data(inpL), sizeof(float)*n_vocab*N);

    // return result for just the last token
    embd_w.resize(n_vocab);
    memcpy(embd_w.data(), (float *) ggml_v2_get_data(inpL) + (n_vocab*(N-1)), sizeof(float)*n_vocab);

    if (mem_per_token == 0) {
        mem_per_token = ggml_v2_used_mem(ctx0)/N;
    }
    //printf("used_mem = %zu\n", ggml_v2_used_mem(ctx0));

    ggml_v2_free(ctx0);

    return true;
}