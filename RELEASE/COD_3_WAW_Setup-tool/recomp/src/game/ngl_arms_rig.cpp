// [acct2-bro] 2026-11-22 Camo lane x534: see ngl_arms_rig.h.
#include "ngl_arms_rig.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

namespace cod3::ngl_arms {
namespace {

bool Finite(const Mat4& m) {
  return std::all_of(m.begin(), m.end(), [](float x) { return std::isfinite(x); });
}

std::string Lower(const std::string& s) {
  std::string out;
  for (char c : s) out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  return out;
}

// Black Ops: j_<part>[twist]_<le|ri>[_<n>]
std::string BlackOpsKey(const std::string& name) {
  const std::string n = Lower(name);
  if (n.rfind("j_", 0) != 0) return {};
  // [acct2-bro] 2026-11-22 x540: the side is the name's END, "_le" / "_ri" or "_le_<n>" / "_ri_<n>" (x534 took the
  // first "_ri" anywhere, so "j_ring_ri_1" read as side "ri" at "j_ri|ng..." and every right ring finger was lost)
  size_t side_at = std::string::npos;
  bool left = true;
  for (const char* tag : {"_le", "_ri"}) {
    const size_t len = n.size();
    if (len >= 6 && n.compare(len - 3, 3, tag) == 0) side_at = len - 3;
    else if (len >= 8 && n.compare(len - 5, 3, tag) == 0 && n[len - 2] == '_' && n[len - 1] >= '0' && n[len - 1] <= '9')
      side_at = len - 5;
    if (side_at != std::string::npos) {
      left = tag[1] == 'l';
      break;
    }
  }
  if (side_at == std::string::npos || side_at < 3) return {};
  const std::string part = n.substr(2, side_at - 2);
  const std::string rest = n.substr(side_at + 3);  // "" or "_<n>"
  int number = -1;
  if (rest.size() == 2 && rest[0] == '_' && rest[1] >= '0' && rest[1] <= '9') number = rest[1] - '0';
  else if (!rest.empty()) return {};
  const std::string side = left ? "L " : "R ";
  static const char* const kFingers[][2] = {{"thumb", "0"}, {"index", "1"}, {"mid", "2"}, {"middle", "2"},
                                            {"ring", "3"}, {"pinky", "4"}};
  for (const auto& f : kFingers) {
    if (part != f[0] || number < 0) continue;
    const int k = std::max(number, 1);  // Black Ops counts 1-3 (the thumb 0-3); the Biped Finger1, Finger11, Finger12
    return side + "Finger" + f[1] + (k <= 1 ? std::string() : std::to_string(k - 1));
  }
  if (number >= 0) return {};
  if (part == "clavicle") return side + "Clavicle";
  if (part == "shoulder") return side + "UpperArm";
  if (part == "elbow") return side + "Forearm";
  if (part == "wrist") return side + "Hand";
  if (part == "wristtwist" || part == "elbowtwist") return side + "ForeTwist";
  return {};
}

// 3ds Max Biped: "Bip01 L UpperArm", "Bip01 R Finger12"
std::string BipedKey(const std::string& name) {
  if (name.rfind("Bip01 ", 0) != 0 || name.size() < 9) return {};
  const char side = name[6];
  if ((side != 'L' && side != 'R') || name[7] != ' ') return {};
  std::string part;
  for (size_t i = 8; i < name.size(); ++i)
    if (name[i] != ' ') part.push_back(name[i]);
  return part.empty() ? std::string() : std::string(1, side) + " " + part;
}

// Jacobi on a symmetric 3x3: eigenvalues in w, eigenvectors in v's columns.
void Jacobi3(double a[3][3], double w[3], double v[3][3]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) v[i][j] = i == j ? 1.0 : 0.0;
  for (int sweep = 0; sweep < 60; ++sweep) {
    const double off = std::fabs(a[0][1]) + std::fabs(a[0][2]) + std::fabs(a[1][2]);
    if (off < 1e-14) break;
    for (int p = 0; p < 2; ++p) {
      for (int q = p + 1; q < 3; ++q) {
        if (std::fabs(a[p][q]) < 1e-18) continue;
        const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        const double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
        for (int k = 0; k < 3; ++k) {
          const double kp = a[k][p], kq = a[k][q];
          a[k][p] = c * kp - s * kq;
          a[k][q] = s * kp + c * kq;
        }
        for (int k = 0; k < 3; ++k) {
          const double pk = a[p][k], qk = a[q][k];
          a[p][k] = c * pk - s * qk;
          a[q][k] = s * pk + c * qk;
        }
        for (int k = 0; k < 3; ++k) {
          const double kp = v[k][p], kq = v[k][q];
          v[k][p] = c * kp - s * kq;
          v[k][q] = s * kp + c * kq;
        }
      }
    }
  }
  for (int i = 0; i < 3; ++i) w[i] = a[i][i];
}

double Det3(const double m[3][3]) {
  return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
         m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

// Umeyama: dst ~ s R src + t (column vectors here; turned into a row-vector Mat4 at the end). False if degenerate.
// [acct2-bro] 2026-11-22 x545: fixed_scale above 0 is used for s (the rotation doesn't depend on it; t follows it).
bool Similarity(const std::vector<std::array<double, 3>>& src, const std::vector<std::array<double, 3>>& dst,
                Mat4* out, float* scale, double fixed_scale = 0) {
  const double n = static_cast<double>(src.size());
  double ms[3] = {0, 0, 0}, md[3] = {0, 0, 0};
  for (size_t k = 0; k < src.size(); ++k)
    for (int i = 0; i < 3; ++i) ms[i] += src[k][i] / n, md[i] += dst[k][i] / n;
  double cov[3][3] = {}, var = 0;
  for (size_t k = 0; k < src.size(); ++k) {
    double a[3], b[3];
    for (int i = 0; i < 3; ++i) a[i] = src[k][i] - ms[i], b[i] = dst[k][i] - md[i];
    for (int i = 0; i < 3; ++i) {
      var += a[i] * a[i] / n;
      for (int j = 0; j < 3; ++j) cov[i][j] += b[i] * a[j] / n;
    }
  }
  if (!(var > 1e-9)) return false;
  // SVD of cov from the eigenvectors of cov^T cov
  double ata[3][3], w[3], v[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      ata[i][j] = 0;
      for (int k = 0; k < 3; ++k) ata[i][j] += cov[k][i] * cov[k][j];
    }
  Jacobi3(ata, w, v);
  int order[3] = {0, 1, 2};
  std::sort(order, order + 3, [&](int x, int y) { return w[x] > w[y]; });
  double sv[3], vv[3][3], u[3][3];
  for (int j = 0; j < 3; ++j) {
    sv[j] = std::sqrt(std::max(w[order[j]], 0.0));
    for (int i = 0; i < 3; ++i) vv[i][j] = v[i][order[j]];
  }
  if (!(sv[1] > 1e-9 * std::max(sv[0], 1e-30))) return false;  // the joints lie on a line: no rotation
  for (int j = 0; j < 2; ++j) {
    double col[3], norm = 0;
    for (int i = 0; i < 3; ++i) {
      col[i] = 0;
      for (int k = 0; k < 3; ++k) col[i] += cov[i][k] * vv[k][j];
      norm += col[i] * col[i];
    }
    norm = std::sqrt(norm);
    for (int i = 0; i < 3; ++i) u[i][j] = col[i] / norm;
  }
  // the third column: orthogonal to the first two (a flat set of joints leaves it free)
  u[0][2] = u[1][0] * u[2][1] - u[2][0] * u[1][1];
  u[1][2] = u[2][0] * u[0][1] - u[0][0] * u[2][1];
  u[2][2] = u[0][0] * u[1][1] - u[1][0] * u[0][1];
  {
    double col[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i)
      for (int k = 0; k < 3; ++k) col[i] += cov[i][k] * vv[k][2];
    double dot = col[0] * u[0][2] + col[1] * u[1][2] + col[2] * u[2][2];
    sv[2] = std::fabs(dot);
    if (dot < 0)
      for (int i = 0; i < 3; ++i) u[i][2] = -u[i][2];
  }
  const double d = Det3(u) * Det3(vv) > 0 ? 1.0 : -1.0;
  const double dd[3] = {1.0, 1.0, d};
  double r[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      r[i][j] = 0;
      for (int k = 0; k < 3; ++k) r[i][j] += u[i][k] * dd[k] * vv[j][k];
    }
  const double s = fixed_scale > 0 ? fixed_scale : (sv[0] * dd[0] + sv[1] * dd[1] + sv[2] * dd[2]) / var;
  double t[3];
  for (int i = 0; i < 3; ++i) {
    t[i] = md[i];
    for (int j = 0; j < 3; ++j) t[i] -= s * r[i][j] * ms[j];
  }
  // column form p' = s R p + t  ->  row form p' = p M with M[j][i] = s R[i][j]
  Mat4 m{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) m[4 * j + i] = static_cast<float>(s * r[i][j]);
  for (int i = 0; i < 3; ++i) m[12 + i] = static_cast<float>(t[i]);
  m[15] = 1.0f;
  *out = m;
  *scale = static_cast<float>(s);
  return Finite(m) && s > 0;
}

void Apply(const Mat4& m, const float p[3], float w, float out[3]) {  // w = 1 a point, 0 a direction
  for (int c = 0; c < 3; ++c) out[c] = p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + w * m[12 + c];
}

// The smallest rotation taking direction a to direction b (Rodrigues), as a row-vector Mat4; identity if either is
// (near) zero. Opposite directions: a half turn about any axis square to a.
Mat4 Turn(const float a_in[3], const float b_in[3]) {
  Mat4 m{};
  m[0] = m[5] = m[10] = m[15] = 1.0f;
  const double la = std::sqrt(double(a_in[0]) * a_in[0] + double(a_in[1]) * a_in[1] + double(a_in[2]) * a_in[2]);
  const double lb = std::sqrt(double(b_in[0]) * b_in[0] + double(b_in[1]) * b_in[1] + double(b_in[2]) * b_in[2]);
  if (!(la > 1e-6) || !(lb > 1e-6)) return m;
  double a[3], b[3];
  for (int i = 0; i < 3; ++i) a[i] = a_in[i] / la, b[i] = b_in[i] / lb;
  double k[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
  const double c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  double sn = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
  if (sn < 1e-9) {
    if (c > 0) return m;
    // a half turn: an axis square to a
    const double pick[3] = {std::fabs(a[0]) < 0.9 ? 1.0 : 0.0, std::fabs(a[0]) < 0.9 ? 0.0 : 1.0, 0.0};
    k[0] = a[1] * pick[2] - a[2] * pick[1];
    k[1] = a[2] * pick[0] - a[0] * pick[2];
    k[2] = a[0] * pick[1] - a[1] * pick[0];
    sn = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
  }
  for (double& x : k) x /= sn;
  const double angle = std::atan2(sn > 1e-9 ? std::sqrt(std::max(0.0, 1 - c * c)) : 0.0, c);
  const double cs = std::cos(angle), si = std::sin(angle), v = 1 - cs;
  // column form R; row form m[4 j + i] = R[i][j]
  const double r[3][3] = {{cs + k[0] * k[0] * v, k[0] * k[1] * v - k[2] * si, k[0] * k[2] * v + k[1] * si},
                          {k[1] * k[0] * v + k[2] * si, cs + k[1] * k[1] * v, k[1] * k[2] * v - k[0] * si},
                          {k[2] * k[0] * v - k[1] * si, k[2] * k[1] * v + k[0] * si, cs + k[2] * k[2] * v}};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) m[4 * j + i] = static_cast<float>(r[i][j]);
  return m;
}

// [acct2-bro] 2026-11-22 Camo lane x540: Call of Duty 3's first-person arms have no bone names at run time (x533:
// every c518 name "-"), so when too few bones pair by name they pair by the skeleton's shape (x539's topo539, which
// on the PC found both hands of the campaign's arms, 5 fingers each, and cod3mp's, 3 two-bone fingers each):
//   - a hand is a game bone with 3 or more finger chains (runs of 2 or more bones with one child each);
//   - its forearm, upper arm and clavicle are its ancestors, a helper (a bone within 0.5 units of its own parent)
//     skipped for the first two;
//   - each game hand is fitted to each Black Ops hand by its palm alone (PalmFit: the hand joint and the fingers'
//     first two joints, over every one-to-one choice of fingers), which the arms' poses and proportions don't touch
//     and a mirror can't pass; the way round with the better two palms gives the sides and the fingers (Black Ops
//     fingers past the game's count stay unpaired and ride the hand).
struct ShapeHand {
  int arm[4] = {-1, -1, -1, -1};  // clavicle, upper arm, forearm, hand
  std::vector<std::vector<int>> fingers;
};

std::vector<ShapeHand> GameHands(const std::vector<GameBone>& game) {
  const size_t n = game.size();
  std::vector<std::vector<int>> kids(n);
  for (size_t i = 0; i < n; ++i)
    if (game[i].parent >= 0 && static_cast<size_t>(game[i].parent) < n && static_cast<size_t>(game[i].parent) != i)
      kids[game[i].parent].push_back(static_cast<int>(i));
  std::vector<std::array<float, 3>> at(n);
  for (size_t i = 0; i < n; ++i) at[i] = OriginOf(game[i].inverse_bind);
  auto chain = [&](int i, std::vector<int>* out) {  // a run with one child each, ending at a leaf
    out->clear();
    for (size_t guard = 0; guard <= n; ++guard) {
      out->push_back(i);
      if (kids[i].empty()) return true;
      if (kids[i].size() != 1) return false;
      i = kids[i][0];
    }
    return false;
  };
  auto too_far = [&](int a, int b) {
    double d = 0;
    for (int c = 0; c < 3; ++c) d += double(at[a][c] - at[b][c]) * (at[a][c] - at[b][c]);
    return d >= 0.25;
  };
  std::vector<std::pair<size_t, int>> found;  // finger count, hand
  std::vector<std::vector<std::vector<int>>> fingers_of(n);
  for (size_t i = 0; i < n; ++i) {
    std::vector<int> c;
    for (int k : kids[i])
      if (chain(k, &c) && c.size() >= 2) fingers_of[i].push_back(c);
    if (fingers_of[i].size() >= 3) found.push_back({fingers_of[i].size(), static_cast<int>(i)});
  }
  std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<ShapeHand> hands;
  for (size_t h = 0; h < found.size() && hands.size() < 2; ++h) {
    ShapeHand hand;
    int walk = found[h].second, filled = 3;
    hand.arm[3] = walk;
    for (size_t guard = 0; guard <= n && filled > 0; ++guard) {
      const int p = game[walk].parent;
      if (p < 0 || static_cast<size_t>(p) >= n) break;
      walk = p;
      const int q = game[p].parent;
      if (filled > 1 && q >= 0 && static_cast<size_t>(q) < n && !too_far(p, q)) continue;  // a helper on its parent
      hand.arm[--filled] = p;
    }
    if (filled > 0) continue;
    hand.fingers = fingers_of[found[h].second];
    hands.push_back(std::move(hand));
  }
  return hands;
}

// One game hand against one Black Ops hand: the hand joint and each finger's first two joints fitted with one
// similarity (the palm alone, so the arms' poses and lengths don't matter), over every one-to-one choice of fingers;
// the smallest error and its choice (for each game finger, the Black Ops finger 0-4, or -1). A rotation can't turn
// a left hand into a right one, so a hand on the wrong side fits badly.
double PalmFit(const Model& model, const std::vector<GameBone>& game, const ShapeHand& hand,
               const std::function<int(const std::string&)>& find, const std::string& side, std::vector<int>* choice) {
  const int wrist = find(side + "Hand");
  if (wrist < 0) return -1;
  std::vector<int> bo_fingers;  // Black Ops fingers with at least a first joint
  for (int f = 0; f < 5; ++f)
    if (find(side + "Finger" + std::to_string(f)) >= 0) bo_fingers.push_back(f);
  const size_t g = std::min<size_t>(hand.fingers.size(), 6);
  if (bo_fingers.size() < 2 || g < 2) return -1;
  double best = -1;
  std::vector<int> pick(g, -1), used(5, 0);
  // every one-to-one choice: game finger c takes Black Ops finger pick[c], or none when the game has more fingers
  std::function<void(size_t, size_t)> walk = [&](size_t c, size_t taken) {
    if (c == g) {
      if (taken < std::min(g, bo_fingers.size())) return;
      std::vector<std::array<double, 3>> src, dst;
      auto add = [&](int bo, int gb) {
        src.push_back({model.bones[bo].pos[0], model.bones[bo].pos[1], model.bones[bo].pos[2]});
        const auto o = OriginOf(game[gb].inverse_bind);
        dst.push_back({o[0], o[1], o[2]});
      };
      add(wrist, hand.arm[3]);
      for (size_t k = 0; k < g; ++k) {
        if (pick[k] < 0) continue;
        for (size_t depth = 0; depth < 2 && depth < hand.fingers[k].size(); ++depth) {
          const int bo = find(side + "Finger" + std::to_string(pick[k]) + (depth ? std::to_string(depth) : ""));
          if (bo >= 0) add(bo, hand.fingers[k][depth]);
        }
      }
      Mat4 m{};
      float scale = 1;
      if (src.size() < 4 || !Similarity(src, dst, &m, &scale)) return;
      double sum = 0;
      for (size_t k = 0; k < src.size(); ++k) {
        const float p[3] = {static_cast<float>(src[k][0]), static_cast<float>(src[k][1]),
                            static_cast<float>(src[k][2])};
        float q[3];
        Apply(m, p, 1.0f, q);
        for (int i = 0; i < 3; ++i) sum += (q[i] - dst[k][i]) * (q[i] - dst[k][i]);
      }
      const double rms = std::sqrt(sum / static_cast<double>(src.size())) / scale;  // in Black Ops' units
      if (std::isfinite(rms) && (best < 0 || rms < best)) best = rms, *choice = pick;
      return;
    }
    for (int f : bo_fingers) {
      if (used[f]) continue;
      used[f] = 1, pick[c] = f;
      walk(c + 1, taken + 1);
      used[f] = 0, pick[c] = -1;
    }
    if (g > bo_fingers.size()) walk(c + 1, taken);  // this game finger takes none
  };
  walk(0, 0);
  return best;
}

// For each Black Ops bone, the game bone it pairs with by shape (-1: none); false if the game has no two hands or
// too few joints pair. `error`: the root mean square joint distance of the fit over every pair (the arms' poses
// differ, so this measures them, not the pairing).
bool PairByShape(const Model& model, const std::vector<GameBone>& game, std::vector<int>* pairs, double* error) {
  const std::vector<ShapeHand> hands = GameHands(game);
  if (hands.size() < 2) return false;
  std::vector<std::string> key(model.bones.size());
  for (size_t i = 0; i < model.bones.size(); ++i) key[i] = BlackOpsKey(model.bones[i].name);
  const std::function<int(const std::string&)> find = [&](const std::string& k) {
    for (size_t i = 0; i < key.size(); ++i)
      if (key[i] == k) return static_cast<int>(i);
    return -1;
  };
  // the sides: the way round whose two palms fit better
  std::vector<int> choice[2][2];  // [game hand][0 left, 1 right]
  double fit[2][2];
  for (int h = 0; h < 2; ++h)
    for (int sd = 0; sd < 2; ++sd) fit[h][sd] = PalmFit(model, game, hands[h], find, sd ? "R " : "L ", &choice[h][sd]);
  if (fit[0][0] < 0 || fit[0][1] < 0 || fit[1][0] < 0 || fit[1][1] < 0) return false;
  const int first = fit[0][0] + fit[1][1] <= fit[0][1] + fit[1][0] ? 0 : 1;  // the first game hand's side
  std::vector<int> pair(model.bones.size(), -1);
  std::vector<std::array<double, 3>> src, dst;
  auto add = [&](int bo, int g) {
    if (bo < 0 || pair[bo] >= 0) return;
    pair[bo] = g;
    src.push_back({model.bones[bo].pos[0], model.bones[bo].pos[1], model.bones[bo].pos[2]});
    const auto o = OriginOf(game[g].inverse_bind);
    dst.push_back({o[0], o[1], o[2]});
  };
  static const char* const kArm[4] = {"Clavicle", "UpperArm", "Forearm", "Hand"};
  for (int h = 0; h < 2; ++h) {
    const int sd = h == 0 ? first : 1 - first;
    const std::string side = sd ? "R " : "L ";
    for (int k = 0; k < 4; ++k) add(find(side + kArm[k]), hands[h].arm[k]);
    const std::vector<int>& pick = choice[h][sd];
    for (size_t c = 0; c < pick.size(); ++c) {
      if (pick[c] < 0) continue;
      for (size_t depth = 0; depth < 3 && depth < hands[h].fingers[c].size(); ++depth)
        add(find(side + "Finger" + std::to_string(pick[c]) + (depth ? std::to_string(depth) : "")),
            hands[h].fingers[c][depth]);
    }
  }
  Mat4 g{};
  float scale = 1;
  if (src.size() < 6 || !Similarity(src, dst, &g, &scale)) return false;
  double sum = 0;
  for (size_t k = 0; k < src.size(); ++k) {
    const float p[3] = {static_cast<float>(src[k][0]), static_cast<float>(src[k][1]), static_cast<float>(src[k][2])};
    float q[3];
    Apply(g, p, 1.0f, q);
    for (int c = 0; c < 3; ++c) sum += (q[c] - dst[k][c]) * (q[c] - dst[k][c]);
  }
  if (error) *error = std::sqrt(sum / static_cast<double>(src.size()));
  *pairs = pair;
  return true;
}

// A bone's first paired descendant in the model's order (children come after their parents), or -1.
int FirstPairedDescendant(const Model& model, const std::vector<char>& paired, size_t i) {
  const size_t n = model.bones.size();
  std::vector<char> under(n, 0);
  under[i] = 1;
  for (size_t k = i + 1; k < n; ++k) {
    const int q = model.bones[k].parent;
    if (q < 0 || static_cast<size_t>(q) >= k || !under[q]) continue;
    under[k] = 1;
    if (paired[k]) return static_cast<int>(k);
  }
  return -1;
}

}  // namespace

std::string PairKey(const std::string& bone_name) {
  std::string k = BipedKey(bone_name);
  return k.empty() ? BlackOpsKey(bone_name) : k;
}

Mat4 Mul(const Mat4& a, const Mat4& b) {
  Mat4 m{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float sum = 0;
      for (int k = 0; k < 4; ++k) sum += a[4 * i + k] * b[4 * k + j];
      m[4 * i + j] = sum;
    }
  return m;
}

std::array<float, 3> OriginOf(const Mat4& m) {  // p_bone = p R + t is 0 at p = -t R^T (R orthonormal)
  const float* t = &m[12];
  std::array<float, 3> o{};
  for (int j = 0; j < 3; ++j) o[j] = -(t[0] * m[4 * j + 0] + t[1] * m[4 * j + 1] + t[2] * m[4 * j + 2]);
  return o;
}

bool MakeRig(const Model& model, const std::vector<GameBone>& game, Rig* out, std::string* why, bool reanchor) {
  auto fail = [&](const std::string& w) {
    if (why) *why = w;
    return false;
  };
  if (!out || model.bones.empty() || game.empty()) return fail("no bones");
  for (const GameBone& b : game)
    if (!Finite(b.inverse_bind)) return fail("a game bone's matrix is not finite");
  Rig rig;
  rig.game_bone.assign(model.bones.size(), -1);
  // the game's root: its first bone with no parent
  int game_root = 0;
  for (size_t i = 0; i < game.size(); ++i)
    if (game[i].parent < 0) {
      game_root = static_cast<int>(i);
      break;
    }
  std::vector<std::array<double, 3>> src, dst;
  std::vector<char> paired(model.bones.size(), 0);
  for (size_t i = 0; i < model.bones.size(); ++i) {
    const std::string key = BlackOpsKey(model.bones[i].name);
    if (key.empty()) continue;
    for (size_t j = 0; j < game.size(); ++j) {
      if (BipedKey(game[j].name) != key) continue;
      rig.game_bone[i] = static_cast<int>(j);
      paired[i] = 1;
      ++rig.pairs;
      const auto o = OriginOf(game[j].inverse_bind);
      src.push_back({model.bones[i].pos[0], model.bones[i].pos[1], model.bones[i].pos[2]});
      dst.push_back({o[0], o[1], o[2]});
      break;
    }
  }
  if (rig.pairs < 6) {  // [acct2-bro] 2026-11-22 x540: the game's arms have no names at run time: by shape
    std::vector<int> by_shape;
    double error = 0;
    const int by_name = rig.pairs;
    if (PairByShape(model, game, &by_shape, &error)) {
      rig.game_bone.assign(model.bones.size(), -1);
      std::fill(paired.begin(), paired.end(), 0);
      src.clear(), dst.clear();
      rig.pairs = 0;
      for (size_t i = 0; i < model.bones.size(); ++i) {
        if (by_shape[i] < 0) continue;
        rig.game_bone[i] = by_shape[i];
        paired[i] = 1;
        ++rig.pairs;
        const auto o = OriginOf(game[by_shape[i]].inverse_bind);
        src.push_back({model.bones[i].pos[0], model.bones[i].pos[1], model.bones[i].pos[2]});
        dst.push_back({o[0], o[1], o[2]});
      }
      rig.by_shape = true;
    }
    if (rig.pairs < 6) {
      char text[96];
      std::snprintf(text, sizeof text, "only %d bones pair by name and %d by shape", by_name,
                    rig.by_shape ? rig.pairs : 0);
      return fail(text);
    }
  }
  // unpaired bones ride their nearest paired ancestor (parents come first), or the game's root
  for (size_t i = 0; i < model.bones.size(); ++i) {
    if (rig.game_bone[i] >= 0) continue;
    const int p = model.bones[i].parent;
    rig.game_bone[i] = p >= 0 && static_cast<size_t>(p) < i ? rig.game_bone[p] : game_root;
  }
  if (!Similarity(src, dst, &rig.align, &rig.scale)) return fail("the paired bones don't fix a fit");
  // [acct2-bro] 2026-11-22 x545: G's scale from the bones' lengths, not from the joints' spread. The game binds its
  // arms bent and its hands far apart, so the global fit's scale (c544a: 1.485) made Black Ops' arms about twice too
  // thick (and the forearm too long even after the 0.5 stretch). The median over the paired segments (a paired bone
  // to its first paired descendant: arms and every finger joint) of the game's length over Black Ops' sizes the
  // hands right; the reanchor still lays and stretches each segment onto the game's.
  rig.fit_scale = rig.scale;
  {
    std::vector<double> ratio;
    for (size_t i = 0; i < model.bones.size(); ++i) {
      if (!paired[i]) continue;
      const int child = FirstPairedDescendant(model, paired, i);
      if (child < 0) continue;
      const auto a = OriginOf(game[rig.game_bone[i]].inverse_bind);
      const auto b = OriginOf(game[rig.game_bone[child]].inverse_bind);
      const float* p = model.bones[i].pos;
      const float* q = model.bones[child].pos;
      const double lg = std::sqrt(double(b[0] - a[0]) * (b[0] - a[0]) + double(b[1] - a[1]) * (b[1] - a[1]) +
                                  double(b[2] - a[2]) * (b[2] - a[2]));
      const double lm = std::sqrt(double(q[0] - p[0]) * (q[0] - p[0]) + double(q[1] - p[1]) * (q[1] - p[1]) +
                                  double(q[2] - p[2]) * (q[2] - p[2]));
      if (lg > 1e-6 && lm > 1e-6 && std::isfinite(lg / lm)) ratio.push_back(lg / lm);
    }
    if (ratio.size() >= 3) {
      std::nth_element(ratio.begin(), ratio.begin() + static_cast<std::ptrdiff_t>(ratio.size() / 2), ratio.end());
      const double s = ratio[ratio.size() / 2];
      if (s > 1e-3 && s < 1e3 && !Similarity(src, dst, &rig.align, &rig.scale, s))
        return fail("the paired bones don't fix a fit at their lengths' scale");
    }
  }
  for (size_t k = 0; k < src.size(); ++k) {
    const float p[3] = {static_cast<float>(src[k][0]), static_cast<float>(src[k][1]), static_cast<float>(src[k][2])};
    float q[3];
    Apply(rig.align, p, 1.0f, q);
    const float d = static_cast<float>(std::sqrt((q[0] - dst[k][0]) * (q[0] - dst[k][0]) +
                                                 (q[1] - dst[k][1]) * (q[1] - dst[k][1]) +
                                                 (q[2] - dst[k][2]) * (q[2] - dst[k][2])));
    rig.worst = std::max(rig.worst, d);
  }
  if (!std::isfinite(rig.worst)) return fail("the fit is not finite");
  // each segment onto the game's: a paired bone's vertices turn about its joint (after G) by the smallest rotation
  // that lays its direction to its first paired descendant on the game bone's direction to that one's pair, and the
  // joint goes onto the game's joint: C = T(-joint) Q T(game joint). A leaf takes its parent's Q; an unpaired bone
  // its nearest paired ancestor's C. Same rest poses: every C is (nearly) the identity, so out = v G S as before.
  const size_t n = model.bones.size();
  std::vector<Mat4> turn(n), place(n);
  std::vector<std::array<float, 3>> joint(n);
  for (size_t i = 0; i < n; ++i) Apply(rig.align, model.bones[i].pos, 1.0f, joint[i].data());
  for (size_t i = 0; i < n; ++i) {
    const int p = model.bones[i].parent;
    const bool has_parent = p >= 0 && static_cast<size_t>(p) < i;
    if (!paired[i]) {
      turn[i] = has_parent ? turn[p] : Mat4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
      place[i] = has_parent ? place[p] : turn[i];
      continue;
    }
    const int child = FirstPairedDescendant(model, paired, i);
    const auto at = OriginOf(game[rig.game_bone[i]].inverse_bind);
    // [acct2-bro] 2026-11-22 x540: and stretched along the game's segment to its length (x539: the campaign's
    // forearm is 0.73 of its upper arm, Black Ops' ~0.96), so the segment's skin reaches the game's next joint; at
    // most 2 times longer or shorter
    Mat4 stretch{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (child >= 0) {
      const auto to = OriginOf(game[rig.game_bone[child]].inverse_bind);
      const float d_model[3] = {joint[child][0] - joint[i][0], joint[child][1] - joint[i][1],
                                joint[child][2] - joint[i][2]};
      const float d_game[3] = {to[0] - at[0], to[1] - at[1], to[2] - at[2]};
      turn[i] = Turn(d_model, d_game);
      const double lm = std::sqrt(double(d_model[0]) * d_model[0] + double(d_model[1]) * d_model[1] +
                                  double(d_model[2]) * d_model[2]);
      const double lg = std::sqrt(double(d_game[0]) * d_game[0] + double(d_game[1]) * d_game[1] +
                                  double(d_game[2]) * d_game[2]);
      if (lm > 1e-6 && lg > 1e-6) {
        const double k = std::clamp(lg / lm, 0.5, 2.0) - 1.0;  // S = I + (k - 1) u u^T, u the game's direction
        const double u[3] = {d_game[0] / lg, d_game[1] / lg, d_game[2] / lg};
        for (int r = 0; r < 3; ++r)
          for (int c = 0; c < 3; ++c) stretch[4 * r + c] += static_cast<float>(k * u[r] * u[c]);
      }
    } else {
      turn[i] = has_parent ? turn[p] : Mat4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    }
    Mat4 from{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -joint[i][0], -joint[i][1], -joint[i][2], 1};
    Mat4 onto{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, at[0], at[1], at[2], 1};
    place[i] = Mul(Mul(Mul(from, turn[i]), stretch), onto);
  }
  rig.place.resize(n);
  for (size_t i = 0; i < n; ++i) {
    rig.place[i] = reanchor ? Mul(rig.align, place[i]) : rig.align;
    if (!Finite(rig.place[i])) return fail("a bone's placement is not finite");
  }
  *out = std::move(rig);
  return true;
}

bool SkinModel(const Model& model, const Rig& rig, const std::vector<Mat4>& skin, size_t stride, size_t normal_at,
               std::vector<float>* out) {
  if (!out || stride < 3 || normal_at + 3 > stride || rig.game_bone.size() != model.bones.size()) return false;
  // each Black Ops bone's whole matrix this frame: its placement (G, then C) times its game bone's skin matrix
  if (rig.place.size() != model.bones.size()) return false;
  std::vector<Mat4> bone(model.bones.size());
  for (size_t i = 0; i < model.bones.size(); ++i) {
    const int g = rig.game_bone[i];
    if (g < 0 || static_cast<size_t>(g) >= skin.size() || !Finite(skin[g])) return false;
    bone[i] = Mul(rig.place[i], skin[g]);
  }
  size_t total = 0;
  for (const Surface& s : model.surfaces) total += s.vertices.size();
  if (out->size() != total * stride) out->assign(total * stride, 0.0f);  // else the rest (RestCorners) is kept
  size_t at = 0;
  for (const Surface& s : model.surfaces) {
    for (const Vertex& v : s.vertices) {
      float* o = &(*out)[at * stride];
      float p[3] = {0, 0, 0}, n[3] = {0, 0, 0}, wsum = 0;
      for (int k = 0; k < 4; ++k) {
        const float w = v.weights[k];
        if (w <= 0) continue;
        if (v.bones[k] >= bone.size()) return false;  // the loader checked it; again here, cheaply
        float a[3], b[3];
        Apply(bone[v.bones[k]], v.pos, 1.0f, a);
        Apply(bone[v.bones[k]], v.normal, 0.0f, b);
        for (int c = 0; c < 3; ++c) p[c] += w * a[c], n[c] += w * b[c];
        wsum += w;
      }
      if (!(wsum > 0)) return false;
      const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      for (int c = 0; c < 3; ++c) {
        o[c] = p[c] / wsum;
        o[normal_at + c] = std::isfinite(len) && len > 1e-12f ? n[c] / len : 0.0f;  // else the face's
      }
      if (!std::isfinite(o[0]) || !std::isfinite(o[1]) || !std::isfinite(o[2])) return false;
      ++at;
    }
  }
  return true;
}

void RestCorners(const Model& model, size_t stride, size_t uv_at, size_t color_at, uint32_t color, size_t normal_at,
                 std::vector<float>* out) {
  size_t total = 0;
  for (const Surface& s : model.surfaces) total += s.vertices.size();
  out->assign(total * stride, 0.0f);
  if (stride < 3 || uv_at + 2 > stride || color_at + 1 > stride || normal_at + 3 > stride) return;
  float color_float;
  std::memcpy(&color_float, &color, 4);
  size_t at = 0;
  for (const Surface& s : model.surfaces) {
    for (const Vertex& v : s.vertices) {
      float* o = &(*out)[at++ * stride];
      const bool finite_normal = std::isfinite(v.normal[0]) && std::isfinite(v.normal[1]) && std::isfinite(v.normal[2]);
      for (int c = 0; c < 3; ++c) o[c] = v.pos[c], o[normal_at + c] = finite_normal ? v.normal[c] : 0.0f;
      o[uv_at] = v.uv[0];
      o[uv_at + 1] = v.uv[1];
      o[color_at] = color_float;
    }
  }
}

}  // namespace cod3::ngl_arms
