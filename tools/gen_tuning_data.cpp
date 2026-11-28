// Emits src/scales/tuning_data.c from Pico2Seq's tuning library (MIT licence).
//
// Build and run from the firmware root, with P set to Pico2Seq's
// src/pico2seq-core directory:
//
//   g++ -std=c++17 -O1 -I$P -include string tools/gen_tuning_data.cpp \
//     $P/tuning/Tuning.cpp $P/tuning/TuningLibrary.cpp \
//     $P/tuning/TuningScales.cpp $P/scales/scales.cpp -o build/gen_tuning_data
//   build/gen_tuning_data > src/scales/tuning_data.c
//
// Pico2Seq's scale numbers are mapped to the firmware's 56-entry table: its
// classic scales to the revised catalog (or -1 if removed), tuned rows to 27 on.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "tuning/Tuning.h"
#include "tuning/TuningScales.h"
#include "scales/scales.h"
using namespace tuning;
int main() {
  const double kFine = 32768.0 / 100.0;
  int fw[SCALES_COUNT];
  const int classic[18] = {0, 2, 5, 4, 3, 1, 6, 11, 19, -1, 15, 9, 25, 26, 19, 22, 23, 24};
  for (int i = 0; i < 18; ++i) fw[i] = classic[i];
  for (int i = 18; i < (int)SCALES_COUNT; ++i) fw[i] = 27 + (i - 18);
  const char *fam[5] = {"TUNING_EQUAL", "TUNING_JUST", "TUNING_TEMPERAMENT", "TUNING_INDIAN", "TUNING_XENO"};
  printf("/* Generated from Pico2Seq's tuning library (src/pico2seq-core/tuning), MIT licence.\n"
         " * Pitches are fine units: 32768 per 12-EDO semitone. */\n"
         "#include \"scales/tuning.h\"\n\n");
  std::vector<bool> equal(libraryCount());
  for (size_t t = 0; t < libraryCount(); ++t) {
    const auto& tu = libraryAt(t);
    const long per = lround(double(tu.periodCents) * kFine);
    bool eq = true;
    for (int i = 0; i < tu.degrees; ++i)
      if (std::labs(lround(double(per) * i / tu.degrees) - lround(double(tu.cents[i]) * kFine)) > 2) eq = false;
    equal[t] = eq;
    if (!eq) {
      printf("static const uint32_t T%d[%d] = {", tu.id, tu.degrees);
      for (int i = 0; i < tu.degrees; ++i) printf("%s%s%ld", i ? "," : "", i % 8 ? " " : "\n\t", lround(double(tu.cents[i]) * kFine));
      printf("\n};\n");
    }
    if (tu.chroma) {
      printf("static const uint8_t C%d[12] = {", tu.id);
      for (int i = 0; i < 12; ++i) printf("%s%d", i ? ", " : "", tu.chroma[i]);
      printf("};\n");
    }
    const bool twelve = tu.degrees == 12 && tu.periodCents == 1200.0f;
    if (!twelve) {
      const ScaleSet set = scaleSet(tu.id);
      printf("static const uint8_t S%d[%d] = {", tu.id, set.count);
      for (int i = 0; i < set.count; ++i) {
        const int scale = fw[set.scales[i]];
        if (scale < 0) {
          fprintf(stderr, "Tuning %d references removed scale %d\n", tu.id, set.scales[i]);
          return EXIT_FAILURE;
        }
        printf("%s%d", i ? ", " : "", scale);
      }
      printf("};\n");
    }
  }
  printf("\nconst struct tuning_def TUNINGS[TUNING_COUNT] = {\n");
  for (size_t t = 0; t < libraryCount(); ++t) {
    const auto& tu = libraryAt(t);
    const long per = lround(double(tu.periodCents) * kFine);
    const bool twelve = tu.degrees == 12 && tu.periodCents == 1200.0f;
    char cents[16], chroma[16], set[16];
    snprintf(cents, sizeof cents, equal[t] ? "0" : "T%d", tu.id);
    snprintf(chroma, sizeof chroma, tu.chroma ? "C%d" : "0", tu.id);
    snprintf(set, sizeof set, twelve ? "0" : "S%d", tu.id);
    int nset = twelve ? 0 : scaleSet(tu.id).count;
    int def = twelve ? (tu.family == Family::Indian ? 19 : 0) : fw[scaleSet(tu.id).scales[0]];
    const double exact = double(tu.periodCents) * kFine;
    long x256 = lround(exact * 256.0);
    printf("\t{%3d, %-19s%2d, %2d, %3d, %6ld, %3ld, %-4s, %-4s, %-4s, \"%s\"},\n", tu.id, (std::string(fam[(int)tu.family]) + ",").c_str(),
           tu.degrees, nset, def, x256 / 256, x256 % 256, cents, chroma, set, tu.name);
  }
  printf("};\n");
}

// Pico2Seq's scales.cpp reads this global; the generator never changes it.
uint8_t currentScale = 0;
