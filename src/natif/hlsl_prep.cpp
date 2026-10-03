// torchlight - préparation des shaders HLSL pour glslang (voir hlsl_prep.h et switch/shaders/prep_hlsl.py).

#include "hlsl_prep.h"

#include <cctype>
#include <map>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace torchlight::natif {
namespace {

// [\s\S] : n'importe quel caractère, retours à la ligne compris (re.S de Python).
const std::regex kMain(R"(void\s+main\s*\(([\s\S]*?)\)\s*\{)");

const char* Suffix(const std::string& type) {
  if (type == "sampler1D") return "1D";
  if (type == "sampler2D") return "2D";
  if (type == "sampler3D") return "3D";
  return "CUBE";
}

std::string Norm(std::string t) {
  std::string out;
  out.reserve(t.size() + 1);
  for (size_t i = 0; i < t.size(); i++) {
    if (t[i] == '\r') {
      out += '\n';
      if (i + 1 < t.size() && t[i + 1] == '\n') i++;
    } else {
      out += t[i];
    }
  }
  if (out.empty() || out.back() != '\n') out += '\n';
  return out;
}

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

bool StartsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// Remplacement par expression régulière avec une fonction (re.sub de Python).
template <typename F>
std::string ReplaceAll(const std::string& text, const std::regex& re, F&& f) {
  std::string out;
  auto begin = std::sregex_iterator(text.begin(), text.end(), re);
  size_t last = 0;
  for (auto it = begin; it != std::sregex_iterator(); ++it) {
    const std::smatch& m = *it;
    out.append(text, last, size_t(m.position(0)) - last);
    out += f(m);
    last = size_t(m.position(0) + m.length(0));
  }
  out.append(text, last, std::string::npos);
  return out;
}

// Entrées du shader de sommets (attributs) : COLORn -> 4+n, TEXCOORDn -> 6+n.
const std::map<std::string, int> kVsIn = {{"POSITION", 0}, {"BLENDWEIGHT", 1}, {"BLENDINDICES", 2}, {"NORMAL", 3},
                                          {"COLOR", 4},    {"TEXCOORD", 6},    {"BINORMAL", 14},    {"TANGENT", 15}};
// Sorties du shader de sommets / entrées du shader de pixels : TEXCOORDn -> n, COLORn -> 8+n.
const std::map<std::string, int> kVarying = {{"TEXCOORD", 0}, {"COLOR", 8}, {"FOG", 10}};

int Location(const std::map<std::string, int>& table, std::string semantic) {
  semantic = Upper(semantic);
  semantic = semantic.substr(0, semantic.find('_'));  // COLOR_center : variante centroïde
  size_t d = semantic.size();
  while (d > 0 && std::isdigit(static_cast<unsigned char>(semantic[d - 1]))) d--;
  const std::string name = semantic.substr(0, d), digits = semantic.substr(d);
  if (name.empty()) return -1;
  for (char c : name)
    if (c < 'A' || c > 'Z') return -1;
  auto it = table.find(name);
  if (it == table.end()) return -1;
  return it->second + (digits.empty() ? 0 : std::stoi(digits));
}

std::string AddLocations(const std::string& t, bool is_vs, std::vector<std::string>& io) {
  std::smatch m;
  if (!std::regex_search(t, m, kMain)) return t;
  static const std::regex param(R"(\b(in|out)\s+([^:,()]+?)\s*:\s*(\w+))");
  const std::string params = ReplaceAll(m.str(1), param, [&](const std::smatch& pm) {
    const std::string direction = pm.str(1), rest = pm.str(2);
    std::string semantic = pm.str(3);
    const std::string sem = Upper(semantic);
    int loc;
    if (direction == "in") {
      loc = is_vs ? Location(kVsIn, sem)
                  : (StartsWith(sem, "POSITION") || StartsWith(sem, "VPOS") ? -1 : Location(kVarying, sem));
    } else {
      loc = StartsWith(sem, "POSITION") || (!is_vs && StartsWith(sem, "COLOR")) || StartsWith(sem, "DEPTH")
                ? -1
                : Location(kVarying, sem);
    }
    std::string prefix;
    if (loc >= 0) {
      prefix = "[[vk::location(" + std::to_string(loc) + ")]] ";
      io.push_back(direction + " " + std::to_string(loc) + " " + sem);
    }
    // Sémantiques système de Direct3D 9 -> équivalents que glslang reconnaît.
    const std::string base_sem = sem.substr(0, sem.find('_'));
    if ((direction == "out" && is_vs && StartsWith(base_sem, "POSITION")) ||
        (direction == "in" && !is_vs && StartsWith(base_sem, "POSITION"))) {
      semantic = "SV_Position";
    } else if (direction == "out" && !is_vs && StartsWith(base_sem, "COLOR")) {
      std::string digits;
      for (char c : base_sem)
        if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
      semantic = "SV_Target" + (digits.empty() ? std::string("0") : digits);
    } else if (direction == "out" && !is_vs && StartsWith(base_sem, "DEPTH")) {
      semantic = "SV_Depth";
    }
    return prefix + direction + " " + rest + " : " + semantic;
  });
  return t.substr(0, size_t(m.position(1))) + params + t.substr(size_t(m.position(1) + m.length(1)));
}

// Test alpha d'Ogre (_setAlphaRejectSettings), absent de Vulkan : ajouté en fin de main() des shaders de pixels.
const char* kAlphaTest =
    "\n"
    "\t{\n"
    "\t\tconst float tl_a = %s.a;\n"
    "\t\tconst float tl_r = torchlight_alpha_test.x;\n"
    "\t\tconst int tl_f = (int)torchlight_alpha_test.y;\n"
    "\t\tbool tl_pass = true;\n"
    "\t\tif (tl_f == 0) tl_pass = false;\n"
    "\t\telse if (tl_f == 2) tl_pass = tl_a < tl_r;\n"
    "\t\telse if (tl_f == 3) tl_pass = tl_a <= tl_r;\n"
    "\t\telse if (tl_f == 4) tl_pass = tl_a == tl_r;\n"
    "\t\telse if (tl_f == 5) tl_pass = tl_a != tl_r;\n"
    "\t\telse if (tl_f == 6) tl_pass = tl_a >= tl_r;\n"
    "\t\telse if (tl_f == 7) tl_pass = tl_a > tl_r;\n"
    "\t\tif (!tl_pass) discard;\n"
    "\t}\n";

std::string AddAlphaTest(const std::string& t) {
  std::smatch m;
  if (!std::regex_search(t, m, kMain)) return t;
  static const std::regex out_re(R"(\bout\s+float4\s+(\w+)\s*:\s*(COLOR0?|SV_Target0?)\b)");
  std::smatch out;
  const std::string params = m.str(1);
  if (!std::regex_search(params, out, out_re)) return t;
  // Accolade fermante de main().
  int depth = 1;
  size_t i = size_t(m.position(0) + m.length(0));
  while (i < t.size() && depth) {
    if (t[i] == '{') depth++;
    if (t[i] == '}') depth--;
    i++;
  }
  const size_t end = i - 1;
  std::string test = kAlphaTest;
  test.replace(test.find("%s"), 2, out.str(1));
  const size_t start = size_t(m.position(0));
  return t.substr(0, start) + "float4 torchlight_alpha_test;\n" + t.substr(start, end - start) + test + t.substr(end);
}

// Constantes globales écrites par le shader (dernier argument d'une fonction FFP_*) : copie locale dans main().
std::string ShadowWrittenGlobals(const std::string& t) {
  std::smatch m;
  if (!std::regex_search(t, m, kMain)) return t;
  const size_t split = size_t(m.position(0) + m.length(0));
  const std::string head = t.substr(0, split);
  std::string body = t.substr(split);
  static const std::regex global_re(R"((?:^|\n)\s*((?:float|half|int)\w*)\s+(\w+)\s*;)");
  std::vector<std::pair<std::string, std::string>> globals;  // nom, type (ordre d'apparition, dernier type gardé)
  for (auto it = std::sregex_iterator(head.begin(), head.end(), global_re); it != std::sregex_iterator(); ++it) {
    const std::string name = (*it).str(2), type = (*it).str(1);
    bool found = false;
    for (auto& g : globals)
      if (g.first == name) {
        g.second = type;
        found = true;
      }
    if (!found) globals.push_back({name, type});
  }
  std::vector<std::pair<std::string, std::string>> written;
  for (const auto& g : globals) {
    const std::regex w(",\\s*" + g.first + R"((\.\w+)?\s*\)\s*;)");
    if (std::regex_search(body, w)) written.push_back(g);
  }
  if (written.empty()) return t;
  std::string decls;
  for (const auto& [name, type] : written) {
    body = std::regex_replace(body, std::regex("\\b" + name + "\\b"), "tl_local_" + name);
    decls += "\n\t" + type + " tl_local_" + name + " = " + name + ";";
  }
  return head + decls + body;
}

}  // namespace

std::string MakeStubSource(const std::string& text) {
  std::smatch m;
  if (!std::regex_search(text, m, kMain)) return {};
  const size_t body_start = size_t(m.position(0) + m.length(0));
  // Fin du corps de main().
  int depth = 1;
  size_t i = body_start;
  while (i < text.size() && depth) {
    if (text[i] == '{') depth++;
    if (text[i] == '}') depth--;
    i++;
  }
  if (depth) return {};
  const std::string head = text.substr(0, size_t(m.position(0)));
  // Constantes globales numériques déclarées avant main (les échantillonneurs ne sont pas dans la table d'Ogre).
  static const std::regex global_re(
      R"((?:^|\n)[ \t]*(?:uniform[ \t]+)?(float|half|int|bool)([1-4](?:x[1-4])?)?[ \t]+(\w+)[ \t]*(\[[^\]]*\])?[ \t]*;)");
  std::string refs;
  for (auto it = std::sregex_iterator(head.begin(), head.end(), global_re); it != std::sregex_iterator(); ++it) {
    const std::string dims = (*it).str(2), name = (*it).str(3), array = (*it).str(4);
    // Tableaux : premier et dernier élément (si le compilateur ne gardait que la partie lue, la taille resterait).
    std::vector<std::string> indices = {""};
    if (!array.empty()) {
      indices = {"[0]"};
      const std::string n = array.substr(1, array.size() - 2);
      if (!n.empty() && n.find_first_not_of("0123456789 ") == std::string::npos && std::stoi(n) > 1)
        indices.push_back("[" + std::to_string(std::stoi(n) - 1) + "]");
    }
    for (const std::string& index : indices) {
      std::string ref = name + index;
      if (dims.find('x') != std::string::npos)
        ref += "[0][0]";
      else if (!dims.empty())
        ref += ".x";
      refs += "\ttl_acc.x += (float)(" + ref + ");\n";
    }
  }
  // Sorties de main : remplies avec l'accumulateur.
  static const std::regex out_re(R"(\bout\s+(float|half)([1-4])?\s+(\w+))");
  std::string outs;
  const std::string params = m.str(1);
  for (auto it = std::sregex_iterator(params.begin(), params.end(), out_re); it != std::sregex_iterator(); ++it) {
    const std::string n = (*it).str(2).empty() ? "1" : (*it).str(2);
    static const char* kSwizzle[] = {"", ".x", ".xy", ".xyz", ""};
    outs += "\t" + (*it).str(3) + " = tl_acc" + kSwizzle[std::stoi(n)] + ";\n";
  }
  if (outs.empty()) return {};
  return text.substr(0, body_start) + "\n\tfloat4 tl_acc = 0;\n" + refs + outs + "}\n" + text.substr(i);
}

std::string PrepareHlslLibraryFile(const std::string& text) {
  static const std::regex re(R"(void\s+FFP_SampleTexture\s*\(\s*in\s+(sampler1D|sampler2D|sampler3D|samplerCUBE)\b)");
  return ReplaceAll(Norm(text), re, [](const std::smatch& m) {
    return "void FFP_SampleTexture" + std::string(Suffix(m.str(1))) + "(in " + m.str(1);
  });
}

std::string PrepareHlslSource(const std::string& text, bool vertex, int64_t d3dx_flags, std::string& io_text) {
  std::string t = Norm(text);
  // Type de chaque échantillonneur, pour réécrire les appels de FFP_SampleTexture.
  std::map<std::string, std::string> types;
  static const std::regex sampler_decl(R"(\b(sampler1D|sampler2D|sampler3D|samplerCUBE)\s+(\w+))");
  for (auto it = std::sregex_iterator(t.begin(), t.end(), sampler_decl); it != std::sregex_iterator(); ++it)
    types[(*it).str(2)] = (*it).str(1);
  static const std::regex call(R"(\bFFP_SampleTexture\s*\(\s*(\w+))");
  t = ReplaceAll(t, call, [&](const std::smatch& m) {
    auto ty = types.find(m.str(1));
    return ty == types.end() ? m.str(0) : "FFP_SampleTexture" + std::string(Suffix(ty->second)) + "(" + m.str(1);
  });
  std::vector<std::string> io;
  if (!vertex) t = AddAlphaTest(t);  // avant AddLocations : la sémantique de sortie est encore COLOR
  t = AddLocations(t, vertex, io);
  t = ShadowWrittenGlobals(t);
  static const std::regex sampler_reg(
      R"(\b(sampler1D|sampler2D|sampler3D|samplerCUBE)\s+\w+\s*:\s*register\s*\(\s*s(\d+)\s*\))");
  for (auto it = std::sregex_iterator(t.begin(), t.end(), sampler_reg); it != std::sregex_iterator(); ++it)
    io.push_back("sampler " + (*it).str(2) + " " + Suffix((*it).str(1)));
  io_text.clear();
  for (size_t k = 0; k < io.size(); k++) io_text += (k ? "\n" : "") + io[k];
  io_text += "\n";
  // Rangement des matrices : même option que le compilateur D3DX du jeu ; sans elle, les programmes animés
  // (world_matrix_array_3x4) sont par lignes, les autres par colonnes.
  const bool row_major =
      d3dx_flags >= 0 ? (d3dx_flags & 8) != 0 : t.find("world_matrix_array_3x4") != std::string::npos;
  if (row_major) t = "#pragma pack_matrix(row_major)\n" + t;
  return t;
}

}  // namespace torchlight::natif
