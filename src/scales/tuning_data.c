/* Generated from Pico2Seq's tuning library (src/pico2seq-core/tuning), MIT licence.
 * Pitches are fine units: 32768 per 12-EDO semitone. */
#include "scales/tuning.h"

static const uint8_t S1[5] = {32, 33, 34, 35, 31};
static const uint8_t S2[4] = {36, 37, 38, 31};
static const uint8_t S3[4] = {39, 40, 41, 31};
static const uint8_t S4[4] = {42, 43, 44, 31};
static const uint8_t S5[3] = {45, 46, 31};
static const uint8_t S6[3] = {47, 48, 31};
static const uint8_t S7[2] = {49, 31};
static const uint8_t S8[1] = {31};
static const uint8_t S9[1] = {31};
static const uint8_t S10[4] = {50, 51, 52, 31};
static const uint8_t S11[4] = {53, 54, 55, 31};
static const uint32_t T32[12] = {
	0, 36612, 66817, 103429, 126587, 163199, 193405, 230017,
	266629, 289787, 326399, 356604
};
static const uint32_t T33[12] = {
	0, 36612, 66817, 87448, 126587, 163199, 190878, 230017,
	266629, 289787, 317465, 356604
};
static const uint32_t T34[12] = {
	0, 37252, 66817, 96382, 133634, 163199, 200452, 230017,
	267269, 296834, 326399, 363651
};
static const uint32_t T35[16] = {
	0, 34392, 66817, 97489, 126587, 154265, 180656, 205873,
	230017, 253175, 275424, 296834, 317465, 337372, 356604, 375205
};
static const uint8_t S35[3] = {56, 57, 31};
static const uint32_t T36[16] = {
	0, 18011, 36612, 55844, 75751, 96382, 117792, 140041,
	163199, 187343, 212560, 238951, 266629, 295727, 326399, 358824
};
static const uint8_t S36[1] = {31};
static const uint32_t T37[43] = {
	0, 7047, 17456, 27678, 36612, 49361, 54069, 59770,
	66817, 75751, 87448, 96382, 103429, 113839, 126587, 136809,
	142568, 154265, 163199, 170247, 180656, 190878, 202338, 212560,
	222969, 230017, 238951, 250648, 256407, 266629, 279377, 289787,
	296834, 305768, 317465, 326399, 333446, 339147, 343855, 356604,
	365538, 375760, 386169
};
static const uint8_t S37[3] = {58, 59, 31};
static const uint32_t T64[12] = {
	0, 24920, 63294, 101668, 126587, 164961, 189881, 228255,
	253175, 291548, 329922, 354842
};
static const uint32_t T65[12] = {
	0, 29565, 62974, 96382, 127869, 163199, 192764, 228095,
	259582, 291068, 326399, 357886
};
static const uint32_t T66[12] = {
	0, 29565, 63294, 96382, 126587, 163199, 193405, 228255,
	259582, 291548, 326399, 356604
};
static const uint32_t T67[12] = {
	0, 30846, 64255, 97663, 128510, 164481, 194046, 228735,
	260863, 292990, 327680, 357245
};
static const uint32_t T96[22] = {
	0, 29565, 36612, 59770, 66817, 96382, 103429, 126587,
	133634, 163199, 170247, 193405, 200452, 230017, 259582, 266629,
	289787, 296834, 326399, 333446, 356604, 363651
};
static const uint8_t C96[12] = {0, 2, 4, 6, 7, 9, 11, 13, 15, 16, 19, 20};
static const uint8_t S96[11] = {16, 27, 28, 29, 0, 2, 5, 4, 3, 1, 31};
static const uint32_t T97[12] = {
	0, 36612, 66817, 103429, 126587, 163199, 193405, 230017,
	266629, 289787, 333446, 356604
};
static const uint32_t T98[12] = {
	0, 29565, 66817, 96382, 133634, 163199, 200452, 230017,
	259582, 296834, 326399, 363651
};
static const uint8_t S128[2] = {60, 31};
static const uint8_t S129[1] = {31};
static const uint8_t S130[1] = {31};
static const uint8_t S131[1] = {31};

const struct tuning_def TUNINGS[TUNING_COUNT] = {
	{  0, TUNING_EQUAL,      12,  0,   0, 393216,   0, 0   , 0   , 0   , "12-EDO (Standard)"},
	{  1, TUNING_EQUAL,      24,  5,  32, 393216,   0, 0   , 0   , S1  , "24-EDO Quarter-Tone"},
	{  2, TUNING_EQUAL,      19,  4,  36, 393216,   0, 0   , 0   , S2  , "19-EDO"},
	{  3, TUNING_EQUAL,      31,  4,  39, 393216,   0, 0   , 0   , S3  , "31-EDO"},
	{  4, TUNING_EQUAL,      22,  4,  42, 393216,   0, 0   , 0   , S4  , "22-EDO"},
	{  5, TUNING_EQUAL,      17,  3,  45, 393216,   0, 0   , 0   , S5  , "17-EDO"},
	{  6, TUNING_EQUAL,      15,  3,  47, 393216,   0, 0   , 0   , S6  , "15-EDO"},
	{  7, TUNING_EQUAL,      10,  2,  49, 393216,   0, 0   , 0   , S7  , "10-EDO"},
	{  8, TUNING_EQUAL,       7,  1,  31, 393216,   0, 0   , 0   , S8  , "7-EDO"},
	{  9, TUNING_EQUAL,       5,  1,  31, 393216,   0, 0   , 0   , S9  , "5-EDO (Slendro)"},
	{ 10, TUNING_EQUAL,      41,  4,  50, 393216,   0, 0   , 0   , S10 , "41-EDO"},
	{ 11, TUNING_EQUAL,      53,  4,  53, 393216,   0, 0   , 0   , S11 , "53-EDO"},
	{ 32, TUNING_JUST,       12,  0,   0, 393216,   0, T32 , 0   , 0   , "Just Intonation"},
	{ 33, TUNING_JUST,       12,  0,   0, 393216,   0, T33 , 0   , 0   , "Just: Harmonic 7"},
	{ 34, TUNING_JUST,       12,  0,   0, 393216,   0, T34 , 0   , 0   , "Pythagorean"},
	{ 35, TUNING_JUST,       16,  3,  56, 393216,   0, T35 , 0   , S35 , "Overtone 16-31"},
	{ 36, TUNING_JUST,       16,  1,  31, 393216,   0, T36 , 0   , S36 , "Undertone 32-17"},
	{ 37, TUNING_JUST,       43,  3,  58, 393216,   0, T37 , 0   , S37 , "Partch 43-Tone"},
	{ 64, TUNING_TEMPERAMENT,12,  0,   0, 393216,   0, T64 , 0   , 0   , "1/4-Comma Meantone"},
	{ 65, TUNING_TEMPERAMENT,12,  0,   0, 393216,   0, T65 , 0   , 0   , "Werckmeister III"},
	{ 66, TUNING_TEMPERAMENT,12,  0,   0, 393216,   0, T66 , 0   , 0   , "Kirnberger III"},
	{ 67, TUNING_TEMPERAMENT,12,  0,   0, 393216,   0, T67 , 0   , 0   , "Vallotti"},
	{ 96, TUNING_INDIAN,     22, 11,  16, 393216,   0, T96 , C96 , S96 , "22 Shruti (Danielou)"},
	{ 97, TUNING_INDIAN,     12,  0,  16, 393216,   0, T97 , 0   , 0   , "12 Svara JI"},
	{ 98, TUNING_INDIAN,     12,  0,  16, 393216,   0, T98 , 0   , 0   , "Pythagorean Svara"},
	{128, TUNING_XENO,       13,  2,  60, 623232, 154, 0   , 0   , S128, "Bohlen-Pierce 13"},
	{129, TUNING_XENO,        1,  1,  31,  25559,  10, 0   , 0   , S129, "Carlos Alpha"},
	{130, TUNING_XENO,        1,  1,  31,  20905, 252, 0   , 0   , S130, "Carlos Beta"},
	{131, TUNING_XENO,        1,  1,  31,  11501, 145, 0   , 0   , S131, "Carlos Gamma"},
};
