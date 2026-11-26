BC_TABLE(int, kBcWeights, 28)
    0, 21, 43, 64,
    0, 9, 18, 27, 37, 46, 55, 64,
    0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64
BC_TABLE_END

BC_TABLE(uint, kBcShapes, 64)
    0xccccu, 0x8888u, 0xeeeeu, 0xecc8u, 0xc880u, 0xfeecu, 0xfec8u, 0xec80u,
    0xc800u, 0xffecu, 0xfe80u, 0xe800u, 0xffe8u, 0xff00u, 0xfff0u, 0xf000u,
    0xf710u, 0x008eu, 0x7100u, 0x08ceu, 0x008cu, 0x7310u, 0x3100u, 0x8cceu,
    0x088cu, 0x3110u, 0x6666u, 0x366cu, 0x17e8u, 0x0ff0u, 0x718eu, 0x399cu,
    0xaaaau, 0xf0f0u, 0x5a5au, 0x33ccu, 0x3c3cu, 0x55aau, 0x9696u, 0xa55au,
    0x73ceu, 0x13c8u, 0x324cu, 0x3bdcu, 0x6996u, 0xc33cu, 0x9966u, 0x0660u,
    0x0272u, 0x04e4u, 0x4e40u, 0x2720u, 0xc936u, 0x936cu, 0x39c6u, 0x639cu,
    0x9336u, 0x9cc6u, 0x817eu, 0xe718u, 0xccf0u, 0x0fccu, 0x7744u, 0xee22u
BC_TABLE_END

BC_TABLE(int, kBcAnchors, 64)
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
    6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15
BC_TABLE_END

BC_TABLE(int, kBcPairs, 20)
    0, 1, 0, 2, 0, 3, 1, 2, 1, 3, 2, 3, 0, 0, 1, 1, 2, 2, 3, 3
BC_TABLE_END

BC_TABLE(int, kBcThree, 12)
    0, 1, 3, 0, 1, 2, 1, 2, 3, 0, 2, 3
BC_TABLE_END

BC_TABLE(int, kBcPlans, 35)
    5, 2, 2, 2, 3,
    6, 4, 4, 0, 0,
    4, 2, 3, 4, 5,
    4, 3, 2, 4, 5,
    3, 2, 2, 0, 0,
    6, 4, 4, 0, 0,
    1, 3, 3, 1, 1
BC_TABLE_END

const int kBcUnique = 0;
const int kBcShared = 1;
const int kBcPlain = 2;
const int kBcByte = 3;
const int kBcFive = 4;
const int kBcSix = 5;

const int kBcHalves = 0;
const int kBcPaint = 1;
const int kBcPlanar = 2;

int BcWeight(int bits, int level) {
  return kBcWeights[(1 << bits) - 4 + level];
}

int BcLerp(int a, int b, int weight) {
  return ((64 - weight) * a + weight * b + 32) >> 6;
}

bool BcHas(uint mask, int pixel) {
  return (mask & (1u << uint(pixel))) != 0u;
}

bool BcIn(int mask, int channel) {
  return (mask & (1 << channel)) != 0;
}

int BcGet(uint color, int channel) {
  return int((color >> uint(channel * 8)) & 255u);
}

uint BcSet(uint color, int channel, int value) {
  return (color & ~(255u << uint(channel * 8))) | (uint(value) << uint(channel * 8));
}

uint BcOf(uint a, uint b, uint c, uint d, int index) {
  return index == 0 ? a : (index == 1 ? b : (index == 2 ? c : d));
}

int BcOfInt(int a, int b, int c, int d, int index) {
  return index == 0 ? a : (index == 1 ? b : (index == 2 ? c : d));
}

int BcLevel(uint low, uint high, int pixel) {
  return int(((pixel < 8 ? low : high) >> uint((pixel & 7) * 4)) & 15u);
}

void BcSetLevel(BC_INOUT(uint, low), BC_INOUT(uint, high), int pixel, int level) {
  const uint shift = uint((pixel & 7) * 4);
  if (pixel < 8) {
    low = (low & ~(15u << shift)) | (uint(level) << shift);
  } else {
    high = (high & ~(15u << shift)) | (uint(level) << shift);
  }
}

int BcScaled(int value, int factor, int scale) {
  return factor * (value / scale) + (factor * (value % scale) + scale / 2) / scale;
}

int BcSnap(int value, int codec, int parity) {
  if (codec == kBcUnique) {
    const int snapped = (((value - parity + 1) >> 1) << 1) + parity;
    return snapped > 255 ? snapped - 2 : snapped;
  }
  const int bits = codec == kBcFive ? 5 : (codec == kBcPlain ? 7 : 6);
  int best = 0;
  int best_gap = 1024;
  for (int nudge = -1; nudge <= 1; ++nudge) {
    int field = clamp((value >> (8 - bits)) + nudge, 0, (1 << bits) - 1);
    int wide = (field << (8 - bits)) | (field >> (2 * bits - 8));
    if (codec == kBcShared) {
      field = (field << 1) | parity;
      wide = (field << 1) | (field >> 6);
    }
    const int gap = abs(wide - value);
    if (gap < best_gap) {
      best_gap = gap;
      best = wide;
    }
  }
  return best;
}

void BcSnapEnds(BC_INOUT(uint, low), BC_INOUT(uint, high), int mask, int codec, int forced) {
  if (codec == kBcByte) {
    return;
  }
  const bool parities = codec == kBcUnique || codec == kBcShared;
  uint even_low = low;
  uint odd_low = low;
  uint even_high = high;
  uint odd_high = high;
  int even_low_error = 0;
  int odd_low_error = 0;
  int even_high_error = 0;
  int odd_high_error = 0;
  for (int c = 0; c < 4; ++c) {
    if (!BcIn(mask, c)) {
      continue;
    }
    for (int trial = 0; trial < 4; trial += parities ? 1 : 2) {
      const int value = BcGet(trial < 2 ? low : high, c);
      const int snapped = BcSnap(value, codec, trial & 1);
      const int gap = (snapped - value) * (snapped - value);
      if (trial == 0) {
        even_low = BcSet(even_low, c, snapped);
        even_low_error += gap;
      } else if (trial == 1) {
        odd_low = BcSet(odd_low, c, snapped);
        odd_low_error += gap;
      } else if (trial == 2) {
        even_high = BcSet(even_high, c, snapped);
        even_high_error += gap;
      } else {
        odd_high = BcSet(odd_high, c, snapped);
        odd_high_error += gap;
      }
    }
  }
  bool low_odd = odd_low_error < even_low_error;
  bool high_odd = odd_high_error < even_high_error;
  if (codec == kBcShared) {
    low_odd = odd_low_error + odd_high_error < even_low_error + even_high_error;
    high_odd = low_odd;
  }
  if (forced >= 0) {
    low_odd = forced == 1;
    high_odd = low_odd;
  }
  low = parities && low_odd ? odd_low : even_low;
  high = parities && high_odd ? odd_high : even_high;
}

void BcSolid(uint color, int mask, int codec, int forced, BC_INOUT(uint, low), BC_INOUT(uint, high)) {
  int best = 0x7fffffff;
  const uint first_low = low;
  const uint first_high = high;
  for (int parity = 0; parity < 2; ++parity) {
    if (forced >= 0 && parity != forced) {
      continue;
    }
    int misses = 0;
    uint trial_low = first_low;
    uint trial_high = first_high;
    for (int c = 0; c < 4; ++c) {
      if (!BcIn(mask, c)) {
        continue;
      }
      const int value = BcGet(color, c);
      int start = value;
      int finish = value;
      if (codec == kBcPlain) {
        start = BcSnap(value, kBcPlain, 0);
        finish = start;
        if (start != value) {
          const int below = value == 128 ? 126 : value - 1;
          const int above = value == 127 ? 129 : value + 1;
          const bool rising = BcLerp(below, above, 21) == value;
          start = rising ? below : above;
          finish = rising ? above : below;
        }
      } else if (codec == kBcUnique && (value & 1) != parity) {
        if (value >= 1 && value <= 254) {
          start = value - 1;
          finish = value + 1;
        } else {
          start = value == 0 ? 1 : 254;
          finish = start;
          ++misses;
        }
      }
      trial_low = BcSet(trial_low, c, start);
      trial_high = BcSet(trial_high, c, finish);
    }
    if (misses < best) {
      best = misses;
      low = trial_low;
      high = trial_high;
    }
  }
}

int BcAssign(BC_IN_ARRAY(uint, px, 16), uint members, uint focus, int mask, int bits, uint low, uint high,
             BC_INOUT(uint, levels_low), BC_INOUT(uint, levels_high)) {
  const int steps = 1 << bits;
  int total = 0;
  for (int i = 0; i < 16; ++i) {
    if (!BcHas(members, i)) {
      continue;
    }
    const int weight = BcHas(focus, i) ? 8 : 1;
    int best = 0x7fffffff;
    int pick = 0;
    for (int level = 0; level < steps; ++level) {
      const int blend = BcWeight(bits, level);
      int error = 0;
      for (int c = 0; c < 4; ++c) {
        if (BcIn(mask, c)) {
          const int gap = BcGet(px[i], c) - BcLerp(BcGet(low, c), BcGet(high, c), blend);
          error += (c == 3 ? 8 : weight) * gap * gap;
        }
      }
      if (error < best) {
        best = error;
        pick = level;
      }
    }
    BcSetLevel(levels_low, levels_high, i, pick);
    total += best;
  }
  return total;
}

void BcRefine(BC_IN_ARRAY(uint, px, 16), uint members, uint focus, int mask, int bits, uint levels_low,
              uint levels_high, BC_INOUT(uint, low), BC_INOUT(uint, high)) {
  const int top = (1 << bits) - 1;
  for (int c = 0; c < 4; ++c) {
    if (!BcIn(mask, c)) {
      continue;
    }
    int high_high = 0;
    int low_low = 0;
    int high_low = 0;
    int high_sum = 0;
    int low_sum = 0;
    for (int i = 0; i < 16; ++i) {
      if (!BcHas(c == 3 ? members : focus, i)) {
        continue;
      }
      const int toward_high = BcLevel(levels_low, levels_high, i);
      const int toward_low = top - toward_high;
      high_high += toward_high * toward_high;
      low_low += toward_low * toward_low;
      high_low += toward_high * toward_low;
      high_sum += toward_high * BcGet(px[i], c);
      low_sum += toward_low * BcGet(px[i], c);
    }
    const int scale = high_high * low_low - high_low * high_low;
    if (scale <= 0) {
      continue;
    }
    const int start = high_high * low_sum - high_low * high_sum;
    const int finish = low_low * high_sum - high_low * low_sum;
    low = BcSet(low, c, start <= 0 ? 0 : min(255, BcScaled(start, top, scale)));
    high = BcSet(high, c, finish <= 0 ? 0 : min(255, BcScaled(finish, top, scale)));
  }
}

int BcFit(BC_IN_ARRAY(uint, px, 16), uint members, uint fitted, uint weighed, int mask, int bits, int codec,
          int forced, BC_INOUT(uint, low), BC_INOUT(uint, high), BC_INOUT(uint, levels_low),
          BC_INOUT(uint, levels_high)) {
  const uint focus = (members & fitted) != 0u ? members & fitted : members;
  uint least = 0xffffffffu;
  uint most = 0u;
  int pivot = 0;
  int pivot_span = -1;
  int count = 0;
  for (int i = 0; i < 16; ++i) {
    if (BcHas(focus, i)) {
      ++count;
    }
  }
  for (int c = 0; c < 4; ++c) {
    int lowest = 255;
    int highest = 0;
    for (int i = 0; i < 16; ++i) {
      if (BcHas(c == 3 ? members : focus, i)) {
        lowest = min(lowest, BcGet(px[i], c));
        highest = max(highest, BcGet(px[i], c));
      }
    }
    if (highest < lowest) {
      lowest = 0;
      highest = 0;
    }
    least = BcSet(least, c, lowest);
    most = BcSet(most, c, highest);
    if (BcIn(mask, c) && highest - lowest > pivot_span) {
      pivot_span = highest - lowest;
      pivot = c;
    }
  }
  const bool steady = pivot_span == 0 && (codec == kBcUnique || codec == kBcPlain || codec == kBcByte);
  uint trial_low = low;
  uint trial_high = high;
  if (steady) {
    BcSolid(least, mask, codec, forced, trial_low, trial_high);
  } else {
    int pivot_sum = 0;
    for (int i = 0; i < 16; ++i) {
      if (BcHas(focus, i)) {
        pivot_sum += BcGet(px[i], pivot);
      }
    }
    for (int c = 0; c < 4; ++c) {
      if (!BcIn(mask, c)) {
        continue;
      }
      int sum = 0;
      int together = 0;
      for (int i = 0; i < 16; ++i) {
        if (BcHas(focus, i)) {
          sum += BcGet(px[i], c);
          together += BcGet(px[i], pivot) * BcGet(px[i], c);
        }
      }
      const bool opposed = count * together - pivot_sum * sum < 0;
      trial_low = BcSet(trial_low, c, BcGet(opposed ? most : least, c));
      trial_high = BcSet(trial_high, c, BcGet(opposed ? least : most, c));
    }
  }
  int best = 0x7fffffff;
  uint trial_levels_low = levels_low;
  uint trial_levels_high = levels_high;
  for (int pass = 0; pass < 3; ++pass) {
    if (pass > 0) {
      if (steady || best == 0) {
        break;
      }
      trial_low = low;
      trial_high = high;
      BcRefine(px, members, focus, mask, bits, levels_low, levels_high, trial_low, trial_high);
    }
    if (!steady) {
      BcSnapEnds(trial_low, trial_high, mask, codec, forced);
    }
    const int error =
        BcAssign(px, members, weighed, mask, bits, trial_low, trial_high, trial_levels_low, trial_levels_high);
    if (error >= best) {
      break;
    }
    best = error;
    low = trial_low;
    high = trial_high;
    levels_low = trial_levels_low;
    levels_high = trial_levels_high;
  }
  return best;
}

int BcPolish(uint c0, uint c1, uint c2, uint c3, int n0, int n1, int n2, int n3, int map,
             BC_INOUT(uint, low), BC_INOUT(uint, high)) {
  int best = 0x7fffffff;
  for (int parities = 0; parities < 4 && best != 0; ++parities) {
    uint trial_low = 0u;
    uint trial_high = 0u;
    int total = 0;
    for (int c = 0; c < 3; ++c) {
      int high_high = 0;
      int low_low = 0;
      int high_low = 0;
      int high_sum = 0;
      int low_sum = 0;
      for (int rank = 0; rank < 4; ++rank) {
        const int count = BcOfInt(n0, n1, n2, n3, rank);
        const int toward_high = (map >> (rank * 2)) & 3;
        const int toward_low = 3 - toward_high;
        const int value = BcGet(BcOf(c0, c1, c2, c3, rank), c);
        high_high += count * toward_high * toward_high;
        low_low += count * toward_low * toward_low;
        high_low += count * toward_high * toward_low;
        high_sum += count * toward_high * value;
        low_sum += count * toward_low * value;
      }
      const int scale = high_high * low_low - high_low * high_low;
      const int start = high_high * low_sum - high_low * high_sum;
      const int finish = low_low * high_sum - high_low * low_sum;
      const int raw_low = scale <= 0 || start <= 0 ? 0 : min(255, BcScaled(start, 3, scale));
      const int raw_high = scale <= 0 || finish <= 0 ? 0 : min(255, BcScaled(finish, 3, scale));
      const int near_low = BcSnap(raw_low, kBcUnique, parities & 1);
      const int near_high = BcSnap(raw_high, kBcUnique, parities >> 1);
      int channel_best = 0x7fffffff;
      for (int nudges = 0; nudges < 9; ++nudges) {
        const int start_at = near_low + (nudges % 3) * 2 - 2;
        const int finish_at = near_high + (nudges / 3) * 2 - 2;
        if (start_at < 0 || start_at > 255 || finish_at < 0 || finish_at > 255) {
          continue;
        }
        int error = 0;
        for (int rank = 0; rank < 4; ++rank) {
          const int gap = BcLerp(start_at, finish_at, BcWeight(2, (map >> (rank * 2)) & 3)) -
                          BcGet(BcOf(c0, c1, c2, c3, rank), c);
          error += BcOfInt(n0, n1, n2, n3, rank) * gap * gap;
        }
        if (error < channel_best) {
          channel_best = error;
          trial_low = BcSet(trial_low, c, start_at);
          trial_high = BcSet(trial_high, c, finish_at);
        }
      }
      total += channel_best;
      if (total >= best) {
        break;
      }
    }
    if (total < best) {
      best = total;
      low = trial_low | 0xff000000u;
      high = trial_high | 0xff000000u;
    }
  }
  return best;
}

int BcFitRamp(uint c0, uint c1, uint c2, uint c3, int n0, int n1, int n2, int n3, BC_INOUT(uint, low),
              BC_INOUT(uint, high)) {
  if (n1 > 0 && n0 > 0 && c0 == c1) {
    n1 += n0;
    n0 = 0;
  }
  if (n2 > 0 && n1 > 0 && c1 == c2) {
    n2 += n1;
    n1 = 0;
  } else if (n2 > 0 && n1 == 0 && n0 > 0 && c0 == c2) {
    n2 += n0;
    n0 = 0;
  }
  if (n3 > 0 && n2 > 0 && c2 == c3) {
    n3 += n2;
    n2 = 0;
  } else if (n3 > 0 && n2 == 0 && n1 > 0 && c1 == c3) {
    n3 += n1;
    n1 = 0;
  } else if (n3 > 0 && n2 == 0 && n1 == 0 && n0 > 0 && c0 == c3) {
    n3 += n0;
    n0 = 0;
  }
  const int used = (n0 > 0 ? 1 : 0) + (n1 > 0 ? 1 : 0) + (n2 > 0 ? 1 : 0) + (n3 > 0 ? 1 : 0);
  low = 0xff000000u;
  high = 0xff000000u;
  if (used == 0) {
    return 0;
  }
  if (used == 1) {
    BcSolid(n3 > 0 ? c3 : (n2 > 0 ? c2 : (n1 > 0 ? c1 : c0)), 7, kBcUnique, -1, low, high);
    return 0x55;
  }
  int first = 4;
  int second = -1;
  if (used == 3) {
    const int skipped = n0 == 0 ? 0 : (n1 == 0 ? 1 : (n2 == 0 ? 2 : 3));
    const uint bottom = skipped == 0 ? c1 : c0;
    const uint middle = skipped <= 1 ? c2 : c1;
    const uint top = skipped == 3 ? c2 : c3;
    const int base = BcGet(bottom, 0) + BcGet(bottom, 1) + BcGet(bottom, 2);
    const int part = 1000 * (BcGet(middle, 0) + BcGet(middle, 1) + BcGet(middle, 2) - base);
    const int whole = BcGet(top, 0) + BcGet(top, 1) + BcGet(top, 2) - base;
    first = part < 408 * whole ? 0 : (part < 500 * whole ? 1 : (part < 592 * whole ? 2 : 3));
    second = first == 1 ? 0 : (first == 2 ? 3 : -1);
  }
  int best = 0x7fffffff;
  int best_map = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const int choice = attempt == 0 ? first : second;
    if (choice < 0 || best == 0) {
      break;
    }
    int map = 0;
    int next = 0;
    for (int rank = 0; rank < 4; ++rank) {
      if (BcOfInt(n0, n1, n2, n3, rank) > 0) {
        const int level = used == 2 ? next * 3 : (used == 3 ? kBcThree[choice * 3 + next] : next);
        map |= level << (rank * 2);
        ++next;
      }
    }
    uint trial_low = 0u;
    uint trial_high = 0u;
    const int error = BcPolish(c0, c1, c2, c3, n0, n1, n2, n3, map, trial_low, trial_high);
    if (error < best) {
      best = error;
      best_map = map;
      low = trial_low;
      high = trial_high;
    }
  }
  int levels = best_map;
  for (int rank = 2; rank >= 0; --rank) {
    if (BcOfInt(n0, n1, n2, n3, rank) == 0) {
      const int above = (levels >> ((rank + 1) * 2)) & 3;
      levels = (levels & ~(3 << (rank * 2))) | (above << (rank * 2));
    }
  }
  return levels;
}

uint BcChannels(int mask) {
  return (BcIn(mask, 0) ? 0xffu : 0u) | (BcIn(mask, 1) ? 0xff00u : 0u) | (BcIn(mask, 2) ? 0xff0000u : 0u);
}

uint BcSnapPaint(uint color, int mask, int codec) {
  if (codec == kBcByte) {
    return color;
  }
  const uint ones = (uint(mask) & 1u) | ((uint(mask) & 2u) << 7u) | ((uint(mask) & 4u) << 14u) |
                    ((uint(mask) & 8u) << 21u);
  const int parity = codec == kBcUnique && bitCount(~color & ones) < bitCount(color & ones) ? 1 : 0;
  uint snapped = color;
  for (int c = 0; c < 4; ++c) {
    if (BcIn(mask, c)) {
      snapped = BcSet(snapped, c, BcSnap(BcGet(color, c), codec, parity));
    }
  }
  return snapped;
}

uint BcFreshLines(uint p0, uint p1, uint p2, uint p3, int mask) {
  const uint channels = BcChannels(mask);
  const int i1 = ((p1 ^ p0) & channels) == 0u ? 0 : 1;
  const int i2 = ((p2 ^ p0) & channels) == 0u ? 0 : (((p2 ^ p1) & channels) == 0u ? 1 : 2);
  const int i3 = ((p3 ^ p0) & channels) == 0u
                     ? 0
                     : (((p3 ^ p1) & channels) == 0u ? 1 : (((p3 ^ p2) & channels) == 0u ? 2 : 3));
  uint met = 0u;
  uint fresh = 0u;
  for (int line = 0; line < 10; ++line) {
    const int a = BcOfInt(0, i1, i2, i3, kBcPairs[line * 2]);
    const int b = BcOfInt(0, i1, i2, i3, kBcPairs[line * 2 + 1]);
    const int code = line < 6 ? min(a, b) * 4 + max(a, b) : 16 + a;
    if (!BcHas(met, code)) {
      met |= 1u << uint(code);
      fresh |= 1u << uint(line);
    }
  }
  return fresh;
}

void BcSnapPaints(uint p0, uint p1, uint p2, uint p3, int mask, int codec, BC_INOUT(uint, s0), BC_INOUT(uint, s1),
                  BC_INOUT(uint, s2), BC_INOUT(uint, s3)) {
  for (int k = 0; k < 4; ++k) {
    const uint snapped = BcSnapPaint(BcOf(p0, p1, p2, p3, k), mask, codec);
    if (k == 0) {
      s0 = snapped;
    } else if (k == 1) {
      s1 = snapped;
    } else if (k == 2) {
      s2 = snapped;
    } else {
      s3 = snapped;
    }
  }
}

void BcNearer(uint paint, uint shade, int mask, int level, BC_INOUT(int, best), BC_INOUT(int, pick)) {
  int error = 0;
  for (int c = 0; c < 3; ++c) {
    if (BcIn(mask, c)) {
      const int gap = BcGet(paint, c) - BcGet(shade, c);
      error += gap * gap;
    }
  }
  if (error < best) {
    best = error;
    pick = level;
  }
}

void BcLine(uint p0, uint p1, uint p2, uint p3, uint s0, uint s1, uint s2, uint s3, int n0, int n1, int n2, int n3,
            int line, int mask, int bits, int codec, BC_INOUT(uint, low), BC_INOUT(uint, high),
            BC_INOUT(int, levels), BC_INOUT(uint, costs_low), BC_INOUT(uint, costs_high)) {
  const int a = kBcPairs[line * 2];
  const int b = kBcPairs[line * 2 + 1];
  if (a == b && (codec == kBcUnique || codec == kBcPlain)) {
    low = BcOf(p0, p1, p2, p3, a);
    high = low;
    BcSolid(low, mask, codec, -1, low, high);
  } else if (codec == kBcShared) {
    low = BcOf(p0, p1, p2, p3, a);
    high = BcOf(p0, p1, p2, p3, b);
    BcSnapEnds(low, high, mask, codec, -1);
  } else {
    low = BcOf(s0, s1, s2, s3, a);
    high = BcOf(s0, s1, s2, s3, b);
  }
  const int steps = ((low ^ high) & BcChannels(mask)) == 0u ? 1 : 1 << bits;
  int best0 = 0xffff;
  int best1 = 0xffff;
  int best2 = 0xffff;
  int best3 = 0xffff;
  int pick0 = 0;
  int pick1 = 0;
  int pick2 = 0;
  int pick3 = 0;
  for (int level = 0; level < steps; ++level) {
    const int blend = BcWeight(bits, level);
    uint shade = 0u;
    for (int c = 0; c < 3; ++c) {
      if (BcIn(mask, c)) {
        shade = BcSet(shade, c, BcLerp(BcGet(low, c), BcGet(high, c), blend));
      }
    }
    if (n0 != 0) {
      BcNearer(p0, shade, mask, level, best0, pick0);
    }
    if (n1 != 0) {
      BcNearer(p1, shade, mask, level, best1, pick1);
    }
    if (n2 != 0) {
      BcNearer(p2, shade, mask, level, best2, pick2);
    }
    if (n3 != 0) {
      BcNearer(p3, shade, mask, level, best3, pick3);
    }
  }
  levels = pick0 | (pick1 << 4) | (pick2 << 8) | (pick3 << 12);
  costs_low = uint(n0 != 0 ? best0 : 0) | (uint(n1 != 0 ? best1 : 0) << 16);
  costs_high = uint(n2 != 0 ? best2 : 0) | (uint(n3 != 0 ? best3 : 0) << 16);
}

void BcMins(uint paint, int red, int green, int blue, bool pairs, BC_INOUT(int, best0), BC_INOUT(int, best1),
            BC_INOUT(int, best2)) {
  const int gap0 = BcGet(paint, 0) - red;
  const int gap1 = BcGet(paint, 1) - green;
  const int gap2 = BcGet(paint, 2) - blue;
  const int error0 = gap0 * gap0;
  const int error1 = gap1 * gap1;
  const int error2 = gap2 * gap2;
  best0 = min(best0, pairs ? error1 + error2 : error0);
  best1 = min(best1, pairs ? error0 + error2 : error1);
  best2 = min(best2, pairs ? error0 + error1 : error2);
}

void BcPlaneLines(uint p0, uint p1, uint p2, uint p3, int n0, int n1, int n2, int n3, uint fresh, int bits,
                  int codec, bool pairs, BC_INOUT(int, cost0), BC_INOUT(int, cost1), BC_INOUT(int, cost2),
                  BC_INOUT(int, lines)) {
  uint s0 = 0u;
  uint s1 = 0u;
  uint s2 = 0u;
  uint s3 = 0u;
  BcSnapPaints(p0, p1, p2, p3, 7, codec, s0, s1, s2, s3);
  const int steps = 1 << bits;
  cost0 = 0x7fffffff;
  cost1 = 0x7fffffff;
  cost2 = 0x7fffffff;
  lines = 0;
  for (int line = 0; line < 10; ++line) {
    if (!BcHas(fresh, line)) {
      continue;
    }
    const int a = kBcPairs[line * 2];
    const int b = kBcPairs[line * 2 + 1];
    uint low = BcOf(s0, s1, s2, s3, a);
    uint high = BcOf(s0, s1, s2, s3, b);
    if (a == b && codec == kBcPlain) {
      low = BcOf(p0, p1, p2, p3, a);
      high = low;
      BcSolid(low, 7, codec, -1, low, high);
    }
    int first0 = 0xffff;
    int first1 = 0xffff;
    int first2 = 0xffff;
    int second0 = 0xffff;
    int second1 = 0xffff;
    int second2 = 0xffff;
    int third0 = 0xffff;
    int third1 = 0xffff;
    int third2 = 0xffff;
    int fourth0 = 0xffff;
    int fourth1 = 0xffff;
    int fourth2 = 0xffff;
    for (int level = 0; level < steps; ++level) {
      const int blend = BcWeight(bits, level);
      const int red = BcLerp(BcGet(low, 0), BcGet(high, 0), blend);
      const int green = BcLerp(BcGet(low, 1), BcGet(high, 1), blend);
      const int blue = BcLerp(BcGet(low, 2), BcGet(high, 2), blend);
      if (n0 != 0) {
        BcMins(p0, red, green, blue, pairs, first0, first1, first2);
      }
      if (n1 != 0) {
        BcMins(p1, red, green, blue, pairs, second0, second1, second2);
      }
      if (n2 != 0) {
        BcMins(p2, red, green, blue, pairs, third0, third1, third2);
      }
      if (n3 != 0) {
        BcMins(p3, red, green, blue, pairs, fourth0, fourth1, fourth2);
      }
    }
    const int total0 = n0 * first0 + n1 * second0 + n2 * third0 + n3 * fourth0;
    const int total1 = n0 * first1 + n1 * second1 + n2 * third1 + n3 * fourth1;
    const int total2 = n0 * first2 + n1 * second2 + n2 * third2 + n3 * fourth2;
    if (total0 < cost0) {
      cost0 = total0;
      lines = (lines & ~15) | line;
    }
    if (total1 < cost1) {
      cost1 = total1;
      lines = (lines & ~(15 << 4)) | (line << 4);
    }
    if (total2 < cost2) {
      cost2 = total2;
      lines = (lines & ~(15 << 8)) | (line << 8);
    }
  }
}

int BcLineCost(uint costs_low, uint costs_high, int n0, int n1, int n2, int n3) {
  return n0 * int(costs_low & 0xffffu) + n1 * int(costs_low >> 16) + n2 * int(costs_high & 0xffffu) +
         n3 * int(costs_high >> 16);
}

void BcOrder(uint members, int anchor, int bits, int mask, BC_INOUT(uint, low), BC_INOUT(uint, high),
             BC_INOUT(uint, levels_low), BC_INOUT(uint, levels_high)) {
  const int top = (1 << bits) - 1;
  if (BcLevel(levels_low, levels_high, anchor) * 2 <= top) {
    return;
  }
  for (int c = 0; c < 4; ++c) {
    if (BcIn(mask, c)) {
      const int kept = BcGet(low, c);
      low = BcSet(low, c, BcGet(high, c));
      high = BcSet(high, c, kept);
    }
  }
  for (int i = 0; i < 16; ++i) {
    if (BcHas(members, i)) {
      BcSetLevel(levels_low, levels_high, i, top - BcLevel(levels_low, levels_high, i));
    }
  }
}

void BcPut(BC_INOUT(uint, w0), BC_INOUT(uint, w1), BC_INOUT(uint, w2), BC_INOUT(uint, w3), BC_INOUT(int, cursor),
           int value, int bits) {
  const int word = cursor >> 5;
  const int shift = cursor & 31;
  const uint head = uint(value) << uint(shift);
  const uint tail = shift + bits > 32 ? uint(value) >> uint(32 - shift) : 0u;
  if (word == 0) {
    w0 |= head;
    w1 |= tail;
  } else if (word == 1) {
    w1 |= head;
    w2 |= tail;
  } else if (word == 2) {
    w2 |= head;
    w3 |= tail;
  } else {
    w3 |= head;
  }
  cursor += bits;
}

void BcPack(int mode, int shape, int lone, int wide_lone, uint low, uint high, uint far_low, uint far_high,
            uint levels_low, uint levels_high, uint lone_low, uint lone_high, BC_INOUT_ARRAY(uint, block, 4)) {
  const bool paired = mode == 1 || mode == 3;
  const bool planes = mode == 4 || mode == 5;
  const uint members = paired ? kBcShapes[shape] : 0u;
  const int anchor = paired ? kBcAnchors[shape] : 0;
  int bits = mode == 6 ? 4 : (mode == 1 ? 3 : 2);
  int lone_bits = 2;
  if (mode == 4) {
    bits = wide_lone != 0 ? 2 : 3;
    lone_bits = wide_lone != 0 ? 3 : 2;
  }
  BcOrder(~members & 0xffffu, 0, bits, planes ? 15 & ~(1 << lone) : 15, low, high, levels_low, levels_high);
  if (paired) {
    BcOrder(members, anchor, bits, 15, far_low, far_high, levels_low, levels_high);
  }
  if (planes) {
    BcOrder(0xffffu, 0, lone_bits, 1 << lone, low, high, lone_low, lone_high);
  }
  uint w0 = 0u;
  uint w1 = 0u;
  uint w2 = 0u;
  uint w3 = 0u;
  int cursor = 0;
  for (int field = 0; field < (mode == 4 ? 3 : (paired || planes ? 2 : 1)); ++field) {
    int value = 1 << mode;
    int width = mode + 1;
    if (field == 1) {
      value = paired ? shape : (lone == 3 ? 0 : lone + 1);
      width = paired ? 6 : 2;
    } else if (field == 2) {
      value = wide_lone != 0 ? 0 : 1;
      width = 1;
    }
    BcPut(w0, w1, w2, w3, cursor, value, width);
  }
  const int drop = mode == 4 ? 3 : (mode == 1 ? 2 : 1);
  const int lone_drop = mode == 4 ? 2 : 0;
  const int color_fields = (mode == 6 ? 4 : 3) * (paired ? 4 : 2);
  const int parity_fields = mode == 3 ? 4 : (mode == 6 || mode == 1 ? 2 : 0);
  for (int field = 0; field < color_fields + (planes ? 2 : 0) + parity_fields; ++field) {
    int value = 0;
    int width = 8 - drop;
    if (field < color_fields) {
      const int c = paired ? field >> 2 : field >> 1;
      const int side = paired ? field & 3 : field & 1;
      const uint ends = side == 0 ? low : (side == 1 ? high : (side == 2 ? far_low : far_high));
      value = BcGet(ends, planes && c == lone ? 3 : c) >> drop;
    } else if (planes) {
      value = BcGet(field == color_fields ? low : high, lone) >> lone_drop;
      width = 8 - lone_drop;
    } else {
      const int side = field - color_fields;
      const uint ends = mode == 1 ? (side == 0 ? low : far_low)
                                  : (side == 0 ? low : (side == 1 ? high : (side == 2 ? far_low : far_high)));
      value = (BcGet(ends, 0) >> (mode == 1 ? 1 : 0)) & 1;
      width = 1;
    }
    BcPut(w0, w1, w2, w3, cursor, value, width);
  }
  const bool lone_first = mode == 4 && wide_lone == 0;
  for (int field = 0; field < (planes ? 32 : 16); ++field) {
    const int i = field & 15;
    const bool from_lone = (field >= 16) != lone_first;
    const int width = from_lone ? lone_bits : bits;
    const int level = from_lone ? BcLevel(lone_low, lone_high, i) : BcLevel(levels_low, levels_high, i);
    BcPut(w0, w1, w2, w3, cursor, level, i == 0 || i == anchor ? width - 1 : width);
  }
  block[0] = w0;
  block[1] = w1;
  block[2] = w2;
  block[3] = w3;
}

void BcPackSolid(uint low, uint high, uint valid, BC_INOUT_ARRAY(uint, block, 4)) {
  uint spread = valid & 0xfffeu;
  spread = (spread | (spread << 8)) & 0x00ff00ffu;
  spread = (spread | (spread << 4)) & 0x0f0f0f0fu;
  spread = (spread | (spread << 2)) & 0x33333333u;
  spread = (spread | (spread << 1)) & 0x55555555u;
  const bool level = (low & 0xffffffu) == (high & 0xffffffu);
  const uint red = uint(BcGet(low, 0) >> 1) | (uint(BcGet(high, 0) >> 1) << 7);
  const uint green = uint(BcGet(low, 1) >> 1) | (uint(BcGet(high, 1) >> 1) << 7);
  const uint blue = uint(BcGet(low, 2) >> 1) | (uint(BcGet(high, 2) >> 1) << 7);
  const uint alpha = uint(BcGet(low, 3)) | (uint(BcGet(high, 3)) << 8);
  block[0] = 32u | (red << 8) | (green << 22);
  block[1] = (green >> 10) | (blue << 4) | (alpha << 18);
  block[2] = (alpha >> 14) | (level ? 0u : (spread << 1) | ((valid & 1u) << 2));
  block[3] = 0u;
}

int BcEtcKind(BC_IN_ARRAY(uint, b, 8), bool punchthrough) {
  const bool individual = !punchthrough && (b[3] & 2u) == 0u;
  bool paint = false;
  bool planar = false;
  for (int c = 0; c < 3; ++c) {
    const int delta = int(b[c] & 7u);
    const int sum = int(b[c] >> 3) + (delta >= 4 ? delta - 8 : delta);
    if (sum < 0 || sum > 31) {
      if (c < 2) {
        paint = true;
      } else {
        planar = true;
      }
    }
  }
  if (!individual && paint) {
    return kBcPaint;
  }
  if (!individual && planar) {
    return kBcPlanar;
  }
  return kBcHalves;
}

void BcEncodeColor(BC_IN_ARRAY(uint, px, 16), uint valid, BC_IN_ARRAY(uint, b, 8), bool punchthrough,
                   BC_INOUT_ARRAY(uint, block, 4)) {
  bool opaque = true;
  bool solid = true;
  uint seen = 0u;
  uint seed = 0u;
  for (int i = 15; i >= 0; --i) {
    if (BcHas(valid, i)) {
      seed = px[i];
    }
  }
  for (int i = 0; i < 16; ++i) {
    if (!BcHas(valid, i)) {
      continue;
    }
    opaque = opaque && (px[i] >> 24) == 255u;
    solid = solid && px[i] == seed;
    if ((px[i] >> 24) >= 8u) {
      seen |= 1u << uint(i);
    }
  }
  uint high_plane = 0u;
  uint low_plane = 0u;
  const uint msb = (b[4] << 8) | b[5];
  const uint lsb = (b[6] << 8) | b[7];
  for (int x = 0; x < 4; ++x) {
    for (int y = 0; y < 4; ++y) {
      const uint texel = uint(x * 4 + y);
      high_plane |= ((msb >> texel) & 1u) << uint(y * 4 + x);
      low_plane |= ((lsb >> texel) & 1u) << uint(y * 4 + x);
    }
  }
  const int kind = BcEtcKind(b, punchthrough);
  const bool mixed = !opaque;
  int shape = (b[3] & 1u) != 0u ? 13 : 0;
  if (kind == kBcPlanar) {
    int across = 0;
    int down = 0;
    for (int c = 0; c < 3; ++c) {
      across += abs(BcGet(px[3], c) - BcGet(px[0], c));
      down += abs(BcGet(px[12], c) - BcGet(px[0], c));
    }
    shape = across > down ? 13 : 0;
  }
  int mode = 3;
  int lone = 3;
  int wide_lone = 0;
  uint low = 0xff000000u;
  uint high = 0xff000000u;
  uint far_low = 0xff000000u;
  uint far_high = 0xff000000u;
  uint levels_low = 0u;
  uint levels_high = 0u;
  uint lone_low = 0u;
  uint lone_high = 0u;
  bool done = solid;
  if (solid) {
    for (int part = 0; part < 2; ++part) {
      BcSolid(seed, part == 0 ? 7 : 8, part == 0 ? kBcPlain : kBcByte, -1, low, high);
    }
  }
  const uint m0 = ~high_plane & ~low_plane & valid;
  const uint m1 = ~high_plane & low_plane & valid;
  const uint m2 = high_plane & ~low_plane & valid;
  const uint m3 = high_plane & low_plane & valid;

  if (!solid && !mixed && kind == kBcHalves) {
    done = true;
    const uint members = kBcShapes[shape];
    for (int subset = 0; subset < 2; ++subset) {
      const uint part = subset == 0 ? ~members & 0xffffu : members;
      const uint r0 = m3 & part;
      const uint r1 = m2 & part;
      const uint r2 = m0 & part;
      const uint r3 = m1 & part;
      uint ramp_low = 0u;
      uint ramp_high = 0u;
      const int ranks = BcFitRamp(r0 != 0u ? px[findLSB(r0)] : seed, r1 != 0u ? px[findLSB(r1)] : seed,
                                  r2 != 0u ? px[findLSB(r2)] : seed, r3 != 0u ? px[findLSB(r3)] : seed,
                                  bitCount(r0), bitCount(r1), bitCount(r2), bitCount(r3), ramp_low, ramp_high);
      if (subset == 0) {
        low = ramp_low;
        high = ramp_high;
      } else {
        far_low = ramp_low;
        far_high = ramp_high;
      }
      for (int i = 0; i < 16; ++i) {
        if (BcHas(part & valid, i)) {
          const int rank = BcHas(high_plane, i) ? (BcHas(low_plane, i) ? 0 : 1) : (BcHas(low_plane, i) ? 3 : 2);
          BcSetLevel(levels_low, levels_high, i, (ranks >> (rank * 2)) & 3);
        }
      }
    }
  }

  if (!solid && !mixed && kind == kBcPaint) {
    done = true;
    const uint p0 = m0 != 0u ? px[findLSB(m0)] : seed;
    const uint p1 = m1 != 0u ? px[findLSB(m1)] : seed;
    const uint p2 = m2 != 0u ? px[findLSB(m2)] : seed;
    const uint p3 = m3 != 0u ? px[findLSB(m3)] : seed;
    const int n0 = bitCount(m0);
    const int n1 = bitCount(m1);
    const int n2 = bitCount(m2);
    const int n3 = bitCount(m3);
    const uint fresh = BcFreshLines(p0, p1, p2, p3, 7);
    int best = 0x7fffffff;
    int win_lines = 0;
    int win_masks = 0;
    int win_bits = 0;
    int win_codecs = 0;
    for (int stage = 0; stage < 5 && best != 0; ++stage) {
      if (stage == 0 || stage == 2) {
        const int bits = stage == 0 ? 2 : 3;
        const int codec = stage == 0 ? kBcUnique : kBcShared;
        uint s0 = 0u;
        uint s1 = 0u;
        uint s2 = 0u;
        uint s3 = 0u;
        BcSnapPaints(p0, p1, p2, p3, 7, codec, s0, s1, s2, s3);
        uint table_low[10];
        uint table_high[10];
        for (int line = 0; line < 10; ++line) {
          if (!BcHas(fresh, line)) {
            continue;
          }
          uint line_low = 0u;
          uint line_high = 0u;
          int line_levels = 0;
          uint costs_low = 0u;
          uint costs_high = 0u;
          BcLine(p0, p1, p2, p3, s0, s1, s2, s3, n0, n1, n2, n3, line, 7, bits, codec, line_low, line_high,
                 line_levels, costs_low, costs_high);
          table_low[line] = costs_low;
          table_high[line] = costs_high;
        }
        int cost = 0x7fffffff;
        int picked = 0;
        int one = 0;
        int two = 0;
        for (int trial = 0; trial < 64 && cost != 0; ++trial) {
          const uint members = kBcShapes[trial];
          const int i0 = bitCount(m0 & members);
          const int i1 = bitCount(m1 & members);
          const int i2 = bitCount(m2 & members);
          const int i3 = bitCount(m3 & members);
          int near_cost = 0x7fffffff;
          int far_cost = 0x7fffffff;
          int near_line = 0;
          int far_line = 0;
          for (int line = 0; line < 10; ++line) {
            if (!BcHas(fresh, line)) {
              continue;
            }
            const int outside = BcLineCost(table_low[line], table_high[line], n0 - i0, n1 - i1, n2 - i2, n3 - i3);
            const int inside = BcLineCost(table_low[line], table_high[line], i0, i1, i2, i3);
            if (outside < near_cost) {
              near_cost = outside;
              near_line = line;
            }
            if (inside < far_cost) {
              far_cost = inside;
              far_line = line;
            }
          }
          if (near_cost + far_cost < cost) {
            cost = near_cost + far_cost;
            picked = trial;
            one = near_line;
            two = far_line;
          }
        }
        if (cost < best) {
          best = cost;
          mode = stage == 0 ? 3 : 1;
          shape = picked;
          lone = 3;
          wide_lone = 0;
          win_lines = one | (two << 4);
          win_masks = 7 | (7 << 4);
          win_bits = bits | (bits << 4);
          win_codecs = codec | (codec << 4);
        }
        continue;
      }
      const int flavor = stage == 1 ? 0 : stage - 2;
      const int rest_bits = flavor == 2 ? 3 : 2;
      const int lone_bits = flavor == 1 ? 3 : 2;
      const int rest_codec = flavor == 0 ? kBcPlain : kBcFive;
      const int lone_codec = flavor == 0 ? kBcByte : kBcSix;
      int rest0 = 0;
      int rest1 = 0;
      int rest2 = 0;
      int rest_lines = 0;
      int lone0 = 0;
      int lone1 = 0;
      int lone2 = 0;
      int lone_lines = 0;
      bool both = false;
      for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1 && min(rest0, min(rest1, rest2)) >= best) {
          break;
        }
        int pass0 = 0;
        int pass1 = 0;
        int pass2 = 0;
        int pass_lines = 0;
        BcPlaneLines(p0, p1, p2, p3, n0, n1, n2, n3, fresh, pass == 0 ? rest_bits : lone_bits,
                     pass == 0 ? rest_codec : lone_codec, pass == 0, pass0, pass1, pass2, pass_lines);
        if (pass == 0) {
          rest0 = pass0;
          rest1 = pass1;
          rest2 = pass2;
          rest_lines = pass_lines;
        } else {
          lone0 = pass0;
          lone1 = pass1;
          lone2 = pass2;
          lone_lines = pass_lines;
          both = true;
        }
      }
      if (!both) {
        continue;
      }
      for (int channel = 0; channel < 3 && best != 0; ++channel) {
        const int cost = BcOfInt(rest0, rest1, rest2, 0, channel) + BcOfInt(lone0, lone1, lone2, 0, channel);
        if (cost >= best) {
          continue;
        }
        best = cost;
        mode = flavor == 0 ? 5 : 4;
        shape = 0;
        lone = channel;
        wide_lone = flavor == 1 ? 1 : 0;
        win_lines = ((rest_lines >> (channel * 4)) & 15) | (((lone_lines >> (channel * 4)) & 15) << 4);
        win_masks = (7 & ~(1 << channel)) | ((1 << channel) << 4);
        win_bits = rest_bits | (lone_bits << 4);
        win_codecs = rest_codec | (lone_codec << 4);
      }
    }
    uint one_low = 0u;
    uint one_high = 0u;
    uint two_low = 0u;
    uint two_high = 0u;
    int one_levels = 0;
    int two_levels = 0;
    for (int side = 0; side < 2; ++side) {
      const int side_mask = (win_masks >> (side * 4)) & 15;
      const int side_bits = (win_bits >> (side * 4)) & 15;
      const int side_codec = (win_codecs >> (side * 4)) & 15;
      uint s0 = 0u;
      uint s1 = 0u;
      uint s2 = 0u;
      uint s3 = 0u;
      BcSnapPaints(p0, p1, p2, p3, side_mask, side_codec, s0, s1, s2, s3);
      uint line_low = 0u;
      uint line_high = 0u;
      int line_levels = 0;
      uint costs_low = 0u;
      uint costs_high = 0u;
      BcLine(p0, p1, p2, p3, s0, s1, s2, s3, n0, n1, n2, n3, (win_lines >> (side * 4)) & 15, side_mask, side_bits,
             side_codec, line_low, line_high, line_levels, costs_low, costs_high);
      if (side == 0) {
        one_low = line_low;
        one_high = line_high;
        one_levels = line_levels;
      } else {
        two_low = line_low;
        two_high = line_high;
        two_levels = line_levels;
      }
    }
    int near_levels = one_levels;
    int far_levels = one_levels;
    int lone_levels = 0;
    if (mode == 1 || mode == 3) {
      low = one_low;
      high = one_high;
      far_low = two_low;
      far_high = two_high;
      far_levels = two_levels;
    } else {
      low = BcSet(one_low, lone, BcGet(two_low, lone)) | 0xff000000u;
      high = BcSet(one_high, lone, BcGet(two_high, lone)) | 0xff000000u;
      lone_levels = two_levels;
    }
    const uint members = mode == 1 || mode == 3 ? kBcShapes[shape] : 0u;
    for (int i = 0; i < 16; ++i) {
      if (!BcHas(valid, i)) {
        continue;
      }
      const int k = (BcHas(high_plane, i) ? 2 : 0) + (BcHas(low_plane, i) ? 1 : 0);
      BcSetLevel(levels_low, levels_high, i, ((BcHas(members, i) ? far_levels : near_levels) >> (k * 4)) & 15);
      BcSetLevel(lone_low, lone_high, i, (lone_levels >> (k * 4)) & 15);
    }
  }

  int best = 0x7fffffff;
  const int plans = done ? 0 : (mixed ? (seen == valid ? 4 : 8) : 3);
  const uint members = kBcShapes[shape];
  for (int trial = 0; trial < plans && best != 0; ++trial) {
    const int plan = (mixed ? trial & 3 : 4 + trial) * 5;
    const int plan_mode = kBcPlans[plan];
    const bool paired = plan_mode == 1 || plan_mode == 3;
    uint trial_low = 0xff000000u;
    uint trial_high = 0xff000000u;
    uint trial_far_low = 0xff000000u;
    uint trial_far_high = 0xff000000u;
    uint trial_levels_low = 0u;
    uint trial_levels_high = 0u;
    uint trial_lone_low = 0u;
    uint trial_lone_high = 0u;
    int error = 0;
    for (int part = 0; part < (plan_mode == 6 ? 1 : 2) && error < best; ++part) {
      const uint part_members = !paired ? valid : (part == 0 ? valid & ~members : valid & members);
      const int mask = plan_mode == 6 ? (mixed ? 15 : 7) : (paired || part == 0 ? 7 : 8);
      uint part_low = paired && part == 1 ? trial_far_low : trial_low;
      uint part_high = paired && part == 1 ? trial_far_high : trial_high;
      uint part_levels_low = !paired && part == 1 ? trial_lone_low : trial_levels_low;
      uint part_levels_high = !paired && part == 1 ? trial_lone_high : trial_levels_high;
      error += BcFit(px, part_members, trial >= 4 ? seen : valid, seen, mask, kBcPlans[plan + 1 + part],
                     kBcPlans[plan + 3 + (paired ? 0 : part)], !mixed && plan_mode == 6 ? 1 : -1, part_low,
                     part_high, part_levels_low, part_levels_high);
      if (paired && part == 1) {
        trial_far_low = part_low;
        trial_far_high = part_high;
      } else {
        trial_low = part_low;
        trial_high = part_high;
      }
      if (!paired && part == 1) {
        trial_lone_low = part_levels_low;
        trial_lone_high = part_levels_high;
      } else {
        trial_levels_low = part_levels_low;
        trial_levels_high = part_levels_high;
      }
    }
    if (error >= best) {
      continue;
    }
    best = error;
    mode = plan_mode;
    lone = 3;
    wide_lone = plan_mode == 4 && kBcPlans[plan + 2] == 3 ? 1 : 0;
    low = trial_low;
    high = trial_high;
    far_low = trial_far_low;
    far_high = trial_far_high;
    levels_low = trial_levels_low;
    levels_high = trial_levels_high;
    lone_low = trial_lone_low;
    lone_high = trial_lone_high;
    if (!mixed && plan_mode == 6) {
      low |= 0xff000000u;
      high |= 0xff000000u;
    }
  }
  if (solid) {
    BcPackSolid(low, high, valid, block);
  } else {
    BcPack(mode, shape, lone, wide_lone, low, high, far_low, far_high, levels_low, levels_high, lone_low, lone_high,
           block);
  }
}

void BcEncodeScalar(BC_IN_ARRAY(int, values, 16), uint valid, bool is_signed, BC_INOUT_ARRAY(uint, block, 2)) {
  const int limit = is_signed ? 254 : 255;
  const int span = is_signed ? 8184 : 8188;
  int low = span;
  int high = 0;
  for (int i = 0; i < 16; ++i) {
    if (BcHas(valid, i)) {
      const int wanted = (is_signed ? values[i] + 1023 : values[i]) * 4;
      low = min(low, wanted);
      high = max(high, wanted);
    }
  }
  uint best_levels_low = 0u;
  uint best_levels_high = 0u;
  int best = 0x7fffffff;
  int top = min(limit, (high * limit + span - 1) / span);
  int bottom = min(limit - 1, low * limit / span);
  if (top <= bottom) {
    top = bottom + 1;
  }
  int best_top = top;
  int best_bottom = bottom;
  for (int pass = 0; pass < 3; ++pass) {
    uint trial_low = 0u;
    uint trial_high = 0u;
    int total = 0;
    int high_high = 0;
    int low_low = 0;
    int high_low = 0;
    int high_sum = 0;
    int low_sum = 0;
    for (int i = 0; i < 16; ++i) {
      if (!BcHas(valid, i)) {
        continue;
      }
      const int wanted = (is_signed ? values[i] + 1023 : values[i]) * 4;
      int nearest = 0x7fffffff;
      int pick = 0;
      for (int level = 0; level < 8; ++level) {
        const int shade = ((top * (7 - level) + bottom * level) * span + (7 * limit) / 2) / (7 * limit);
        const int gap = wanted - shade;
        if (gap * gap < nearest) {
          nearest = gap * gap;
          pick = level;
        }
      }
      BcSetLevel(trial_low, trial_high, i, pick);
      total += nearest >> 4;
      const int toward_top = 7 - pick;
      high_high += toward_top * toward_top;
      low_low += pick * pick;
      high_low += toward_top * pick;
      high_sum += toward_top * wanted;
      low_sum += pick * wanted;
    }
    if (total >= best) {
      break;
    }
    best = total;
    best_top = top;
    best_bottom = bottom;
    best_levels_low = trial_low;
    best_levels_high = trial_high;
    const int scale = high_high * low_low - high_low * high_low;
    if (scale <= 0 || best == 0) {
      break;
    }
    const int start = low_low * high_sum - high_low * low_sum;
    const int finish = high_high * low_sum - high_low * high_sum;
    const int fitted_top = start <= 0 ? 0 : min(span, BcScaled(start, 7, scale));
    const int fitted_bottom = finish <= 0 ? 0 : min(span, BcScaled(finish, 7, scale));
    top = min(limit, (fitted_top * limit + span / 2) / span);
    bottom = min(limit, (fitted_bottom * limit + span / 2) / span);
    if (top <= bottom) {
      break;
    }
  }
  const int offset = is_signed ? 127 : 0;
  uint w0 = uint((best_top - offset) & 255) | (uint((best_bottom - offset) & 255) << 8);
  uint w1 = 0u;
  for (int i = 0; i < 16; ++i) {
    const int level = BcLevel(best_levels_low, best_levels_high, i);
    const uint code = uint(level == 0 ? 0 : (level == 7 ? 1 : level + 1));
    const int at = 16 + i * 3;
    if (at < 32) {
      w0 |= code << uint(at);
      if (at > 29) {
        w1 |= code >> uint(32 - at);
      }
    } else {
      w1 |= code << uint(at - 32);
    }
  }
  block[0] = w0;
  block[1] = w1;
}
