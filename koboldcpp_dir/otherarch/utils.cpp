#include "utils.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <regex>
#include <locale>
#include <codecvt>
#include <sstream>


void utreplace(std::string & str, const std::string & needle, const std::string & replacement) {
    size_t pos = 0;
    while ((pos = str.find(needle, pos)) != std::string::npos) {
        str.replace(pos, needle.length(), replacement);
        pos += replacement.length();
    }
}

std::map<std::string, int32_t> json_parse(const std::string & fname) {
    std::map<std::string, int32_t> result;

    // read file into string
    std::string json;
    {
        std::ifstream ifs(fname);
        if (!ifs) {
            fprintf(stderr, "Failed to open %s\n", fname.c_str());
            exit(1);
        }

        json = std::string((std::istreambuf_iterator<char>(ifs)),
                (std::istreambuf_iterator<char>()));
    }

    if (json[0] != '{') {
        return result;
    }

    // parse json
    {
        bool has_key  = false;
        bool in_token = false;

        std::string str_key = "";
        std::string str_val = "";

        int n = json.size();
        for (int i = 1; i < n; ++i) {
            if (!in_token) {
                if (json[i] == ' ') continue;
                if (json[i] == '"') {
                    in_token = true;
                    continue;
                }
            } else {
                if (json[i] == '\\' && i+1 < n) {
                    if (has_key == false) {
                        str_key += json[i];
                    } else {
                        str_val += json[i];
                    }
                    ++i;
                } else if (json[i] == '"') {
                    if (has_key == false) {
                        has_key = true;
                        ++i;
                        while (json[i] == ' ') ++i;
                        ++i; // :
                        while (json[i] == ' ') ++i;
                        if (json[i] != '\"') {
                            while (json[i] != ',' && json[i] != '}') {
                                str_val += json[i++];
                            }
                            has_key = false;
                        } else {
                            in_token = true;
                            continue;
                        }
                    } else {
                        has_key = false;
                    }

                    ::utreplace(str_key, "\\u0120", " " ); // \u0120 -> space
                    ::utreplace(str_key, "\\u010a", "\n"); // \u010a -> new line
                    ::utreplace(str_key, "\\\"",    "\""); // \\\"   -> "

                    try {
                        result[str_key] = std::stoi(str_val);
                    } catch (...) {
                        //fprintf(stderr, "%s: ignoring key '%s' with value '%s'\n", fname.c_str(), str_key.c_str(), str_val.c_str());

                    }
                    str_key = "";
                    str_val = "";
                    in_token = false;
                    continue;
                }
                if (has_key == false) {
                    str_key += json[i];
                } else {
                    str_val += json[i];
                }
            }
        }
    }

    return result;
}


void gpt_vocab::add_special_token(const std::string & token) {
    special_tokens.push_back(token);
}


std::string convert_to_utf8(const std::wstring & input) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.to_bytes(input);
}


std::wstring convert_to_wstring(const std::string & input) {
    try {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        return converter.from_bytes(input);
    } catch (const std::range_error& e) {
        return L"";
    } catch (...) {
        return L"";
    }
}

void gpt_split_words(std::string str, std::vector<std::string>& words) {
    const std::string pattern = R"('s|'t|'re|'ve|'m|'ll|'d| ?[[:alpha:]]+| ?[[:digit:]]+| ?[^\s[:alpha:][:digit:]]+|\s+(?!\S)|\s+)";
    const std::regex re(pattern);
    std::smatch m;

    while (std::regex_search(str, m, re)) {
        for (auto x : m) {
            words.push_back(x);
        }
        str = m.suffix();
    }
}

std::vector<gpt_vocab::id> gpt_tokenize(const gpt_vocab & vocab, const std::string & text) {
    std::vector<std::string> words;

    // first split the text into words
    {
        std::string str = text;

        // Generate the subpattern from the special_tokens vector if it's not empty
        if (!vocab.special_tokens.empty()) {
            const std::regex escape(R"([\[\\\^\$\.\|\?\*\+\(\)\{\}])");
            std::string special_tokens_subpattern;
            for (const auto & token : vocab.special_tokens) {
                if (!special_tokens_subpattern.empty()) {
                    special_tokens_subpattern += "|";
                }
                special_tokens_subpattern += std::regex_replace(token, escape, R"(\$&)");
            }

            std::regex re(special_tokens_subpattern);
            std::smatch m;
            // Split the text by special tokens.
            while (std::regex_search(str, m, re)) {
                // Split the substrings in-between special tokens into words.
                gpt_split_words(m.prefix(), words);
                // Add matched special tokens as words.
                for (auto x : m) {
                    words.push_back(x);
                }
                str = m.suffix();
            }
            // Remaining text without special tokens will be handled below.
        }

        gpt_split_words(str, words);
    }

    // find the longest token that forms each word in words:
    std::vector<gpt_vocab::id> tokens;
    for (const auto & word : words) {
        for (int i = 0; i < word.size(); ){
            for (int j = word.size() - 1; j >= i; j--){
                auto cand = word.substr(i, j-i+1);
                auto it = vocab.token_to_id.find(cand);
                if (it != vocab.token_to_id.end()){ // word.substr(i, j-i+1) in vocab
                    tokens.push_back(it->second);
                    i = j + 1;
                    break;
                }
                else if (j == i){ // word.substr(i, 1) has no matching
                    fprintf(stderr, "%s: unknown token '%s'\n", __func__, word.substr(i, 1).data());
                    i++;
                }
            }
        }
    }


    return tokens;
}

bool should_transpose_layer(std::string name)
{

    if(name.find(".mlp.fc_in.weight")!=std::string::npos ||
    name.find(".attn.out_proj.weight")!=std::string::npos ||
    name.find(".attn.q_proj.weight")!=std::string::npos ||
    name.find(".attn.k_proj.weight")!=std::string::npos ||
    name.find(".attn.v_proj.weight")!=std::string::npos ||
    name.find("/attn/c_attn/w")!=std::string::npos ||
    name.find("/attn/c_proj/w")!=std::string::npos ||
    name.find("/mlp/c_fc/w")!=std::string::npos ||
    name.find("/mlp/c_proj/w")!=std::string::npos)
    {
        return true;
    }
    return false;
}

static std::vector<uint8_t> kcpp_compute_buf;
void kcpp_graph_compute_helper(struct ggml_v3_cgraph *graph, int n_threads)
{
    struct ggml_v3_cplan plan = ggml_v3_graph_plan(graph, n_threads);
    if (plan.work_size > 0)
    {
        kcpp_compute_buf.resize(plan.work_size);
        plan.work_data = kcpp_compute_buf.data();
    }
    ggml_v3_graph_compute(graph, &plan);
}

static const std::string kcpp_base64_chars =
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrstuvwxyz"
             "0123456789+/";
static inline bool kcpp_is_base64(uint8_t c)
{
    return (isalnum(c) || (c == '+') || (c == '/'));
}
std::vector<uint8_t> kcpp_base64_decode(const std::string & encoded_string)
{
    int i = 0;
    int j = 0;
    int in_ = 0;

    int in_len = encoded_string.size();

    uint8_t char_array_4[4];
    uint8_t char_array_3[3];

    std::vector<uint8_t> ret;

    while (in_len-- && (encoded_string[in_] != '=') && kcpp_is_base64(encoded_string[in_]))
    {
        char_array_4[i++] = encoded_string[in_]; in_++;
        if (i == 4)
        {
            for (i = 0; i <4; i++)
            {
                char_array_4[i] = kcpp_base64_chars.find(char_array_4[i]);
            }

            char_array_3[0] = ((char_array_4[0]      ) << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) +   char_array_4[3];

            for (i = 0; (i < 3); i++)
            {
                ret.push_back(char_array_3[i]);
            }
            i = 0;
        }
    }

    if (i)
    {
        for (j = i; j <4; j++)
        {
            char_array_4[j] = 0;
        }

        for (j = 0; j <4; j++)
        {
            char_array_4[j] = kcpp_base64_chars.find(char_array_4[j]);
        }

        char_array_3[0] = ((char_array_4[0]      ) << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        char_array_3[2] = ((char_array_4[2] & 0x3) << 6) +   char_array_4[3];

        for (j = 0; (j < i - 1); j++)
        {
            ret.push_back(char_array_3[j]);
        }
    }

    return ret;
}
