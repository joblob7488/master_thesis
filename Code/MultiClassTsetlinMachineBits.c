/*

Copyright (c) 2019 Ole-Christoffer Granmo

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

This code implements a multiclass version of the Tsetlin Machine from paper arXiv:1804.01508
https://arxiv.org/abs/1804.01508

*/

#include <stdio.h>
#include <stdlib.h>

#include "MultiClassTsetlinMachineBits.h"

#define PADDED_CHUNKS 25

/**************************************/
/*** The Multiclass Tsetlin Machine ***/
/**************************************/

/*** Initialize Tsetlin Machine ***/
struct MultiClassTsetlinMachine *CreateMultiClassTsetlinMachine()
{

	struct MultiClassTsetlinMachine *mc_tm;

	mc_tm = (void *)malloc(sizeof(struct MultiClassTsetlinMachine));

	for (int i = 0; i < CLASSES; i++) {
		mc_tm->tsetlin_machines[i] = CreateTsetlinMachine();
	}
	return mc_tm;
}

void mc_tm_initialize(struct MultiClassTsetlinMachine *mc_tm)
{
	for (int i = 0; i < CLASSES; i++) {
		tm_initialize(mc_tm->tsetlin_machines[i]);
	}
}

/********************************************/
/*** Evaluate the Trained Tsetlin Machine ***/
/********************************************/

float mc_tm_evaluate(struct MultiClassTsetlinMachine *mc_tm, unsigned int X[][LA_CHUNKS], int y[], int number_of_examples)
{
	int errors;
	int max_class;
	int max_class_sum;

	errors = 0;
	for (int l = 0; l < number_of_examples; l++) {
		/******************************************/
		/*** Identify Class with Largest Output ***/
		/******************************************/

		max_class_sum = tm_score(mc_tm->tsetlin_machines[0], X[l]);
		max_class = 0;
		for (int i = 1; i < CLASSES; i++) {	
			int class_sum = tm_score(mc_tm->tsetlin_machines[i], X[l]);
			if (max_class_sum < class_sum) {
				max_class_sum = class_sum;
				max_class = i;
			}
		}

		if (max_class != y[l]) {
			errors += 1;
		}
	}
	
	return 1.0 - 1.0 * errors / number_of_examples;
}

/******************************************/
/*** Online Training of Tsetlin Machine ***/
/******************************************/

// The Tsetlin Machine can be trained incrementally, one training example at a time.
// Use this method directly for online and incremental training.

void mc_tm_update(struct MultiClassTsetlinMachine *mc_tm, unsigned int Xi[], int target_class)
{
	tm_update(mc_tm->tsetlin_machines[target_class], Xi, 1);

	// Randomly pick one of the other classes, for pairwise learning of class output 
	unsigned int negative_target_class = (unsigned int)CLASSES * 1.0*rand()/((unsigned int)RAND_MAX + 1);
	while (negative_target_class == target_class) {
		negative_target_class = (unsigned int)CLASSES * 1.0*rand()/((unsigned int)RAND_MAX + 1);
	}
	tm_update(mc_tm->tsetlin_machines[negative_target_class], Xi, 0);
}

/**********************************************/
/*** Batch Mode Training of Tsetlin Machine ***/
/**********************************************/

void mc_tm_fit(struct MultiClassTsetlinMachine *mc_tm, unsigned int X[][LA_CHUNKS], int y[], int number_of_examples, int epochs)
{
	for (int epoch = 0; epoch < epochs; epoch++) {
		// Add shuffling here...		
		for (int i = 0; i < number_of_examples; i++) {
			mc_tm_update(mc_tm, X[i], y[i]);
		}
	}
}


static unsigned int get_pos_word(const struct TsetlinMachine *tm, int j, int k)
{
    unsigned int word = tm->ta_state[j][k][STATE_BITS - 1];
    if (k == PADDED_CHUNKS - 1) {
        word &= (1u << (FEATURES % INT_SIZE)) - 1u;
    }
    return word;
}

static unsigned int get_neg_word(const struct TsetlinMachine *tm, int j, int k)
{
    int ta_word_lo = 24 + k;      
    int ta_word_hi = 25 + k;      
 
    unsigned int lo = (tm->ta_state[j][ta_word_lo][STATE_BITS - 1] >> 16) & 0xFFFFu;
    unsigned int hi = 0;
 
    if (k < PADDED_CHUNKS - 1) {
        hi = (tm->ta_state[j][ta_word_hi][STATE_BITS - 1] & 0xFFFFu) << 16;
    }
 
    return lo | hi;
}
/*************************************/
/*** Save / Load the trained model ***/
/*************************************/
 
int mc_tm_save(const char *path, const struct MultiClassTsetlinMachine *mc)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "mc_tm_save_cfu: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }
 
    for (int c = 0; c < CLASSES; ++c) {
        const struct TsetlinMachine *tm = mc->tsetlin_machines[c];
        for (int j = 0; j < CLAUSES; ++j) {
 
            for (int k = 0; k < PADDED_CHUNKS; ++k) {
                unsigned int word = get_pos_word(tm, j, k);
                if (fwrite(&word, sizeof(word), 1, fp) != 1) {
                    fclose(fp);
                    return -1;
                }
            }
 
            for (int k = 0; k < PADDED_CHUNKS; ++k) {
                unsigned int word = get_neg_word(tm, j, k);
                if (fwrite(&word, sizeof(word), 1, fp) != 1) {
                    fclose(fp);
                    return -1;
                }
            }
        }
    }
 
    fclose(fp);
    return 0;
}
 
int mc_tm_load_into(const char *path, struct MultiClassTsetlinMachine *mc)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		fprintf(stderr, "mc_tm_load_into: cannot open '%s': %s\n", path, strerror(errno));
		return -1;
	}
 
	for (int c = 0; c < CLASSES; ++c) {
		struct TsetlinMachine *tm = mc->tsetlin_machines[c];
 
		memset(tm->ta_state, 0, sizeof(tm->ta_state));
 
		for (int j = 0; j < CLAUSES; ++j) {
			for (int k = 0; k < LA_CHUNKS; ++k) {
				unsigned int word;
				if (fread(&word, sizeof(word), 1, fp) != 1) {
					fclose(fp);
					return -1;
				}
				tm->ta_state[j][k][STATE_BITS - 1] = word;
			}
		}
	}
 
	fclose(fp);
	return 0;
}
 
struct MultiClassTsetlinMachine *mc_tm_load_new(const char *path)
{
	struct MultiClassTsetlinMachine *mc = CreateMultiClassTsetlinMachine();
	if (!mc) return NULL;
	if (mc_tm_load_into(path, mc) != 0) {
		return NULL;
	}
	return mc;
}