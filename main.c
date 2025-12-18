#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  char **tok;
  size_t count;
  size_t capacity;
} Tokens;

#define MARKOV_DEGREE 5

#define da_append(list, value)                                                 \
  do {                                                                         \
    if ((list).count >= ((list)).capacity) {                                   \
      if ((list).capacity == 0) {                                              \
        (list).capacity = 256;                                                 \
      } else {                                                                 \
        (list).capacity *= 2;                                                  \
      }                                                                        \
      (list).tok = realloc((list).tok, (list).capacity * sizeof(*(list).tok)); \
    }                                                                          \
    (list).tok[(list).count++] = (value);                                      \
  } while (0);

#define MAX_SIZE 2056

size_t size = 0;            // Current number of elements in the map
char keys[MAX_SIZE][100];   // Array to store the keys
Tokens tokens[MAX_SIZE];    // Array to store the tokens

// Function to get the index of key in the keys array
int getIndex(char key[]) {
  for (size_t i = 0; i < size; i++) {
    if (strcmp(keys[i], key) == 0) {
      return i;
    }
  }
  return -1; // Key not found
}

// Function to insert a key-value pair into the map
void insert(char key[], char value[]) {
  int index = getIndex(key);
  if (index == -1) { // Key not found
    strcpy(keys[size], key);

    da_append(tokens[size], value);
    // printf("[KEY NOT FOUND] append %s [TOKEN] %s\n", value,
    // *tokens[size].tok);
    size++;
  } else { // Key found
    da_append(tokens[index], value);
    // printf("[KEY FOUND] append %s at idx %d\n", value, index);
  }
}

// Function to get the value of a key in the map
Tokens *get(char key[]) {
  int index = getIndex(key);
  if (index == -1) { // Key not found
    return NULL;
  } else { // Key found
    return &tokens[index];
  }
}

// Function to print the map
void printMap() {
  for (size_t i = 0; i < size; i++) {
    printf("[KEY, VALUES] : [%s] -> [", keys[i]);
    for (size_t j = 0; j < tokens[i].count; j++) {
      printf(" %s ", tokens[i].tok[j]);
    }
    printf("]\n");
  }
}

void printChain(char **chain, size_t chain_length) {
  for (size_t i = 0; i < chain_length; i++) {
    printf("%s ", chain[i]);
  }
  printf("\n");
}

// function that chooses a random index in an array.
// needs as input the array of strings and the array size in order to cap the
// random value
int randomIdx(size_t max) {
  assert(max > 0);
  size_t idx = rand() % max;
  return idx;
}

// function that creates the map key from a list of words.
// Important to centralize it since it is used both during training and
// inference of the model.
void createKey(char *buffer, char **source, int start_index, int degree) {
  buffer[0] = '\0';
  for (int i = 0; i < degree; i++) {
    strcat(buffer, source[start_index + i]);
    if (i < degree - 1) {
      strcat(buffer, " "); // Use a space consistently
    }
  }
}

// void initChain(char **chain) {
//   char last_state[512] = "";
//   int random_idx = randomIdx(size);
//   Tokens* tokens = &tokens[random_idx];
//   // while (tokens->count == 0) {
//   //     random_idx = randomIdx(size);
//   //     tokens = &tokens[random_idx];
//   // }
//   char* key = keys[random_idx];
//   printf("random key: %s\n", key);
//   printf("random effects: %zu\n", tokens->count);
//   chain[0] = key;
//   // createKey(last_state, tokens, 0, MARKOV_DEGREE);
//   // printf("initial state %s\n", last_state);
// }

void initChain(char **chain) {
    // 1. Pick a random index from the map
    int random_idx = rand() % size;
    char* random_key = keys[random_idx];

    printf("Seeding with key: [%s]\n", random_key);

    // 2. We need a temporary copy because strtok modifies the string
    char temp_key[512];
    strcpy(temp_key, random_key);

    // 3. Split the key by space and fill the chain
    char* word = strtok(temp_key, " ");
    int i = 0;
    while (word != NULL && i < MARKOV_DEGREE) {
        // strdup is important here so the chain owns its own strings
        chain[i] = strdup(word);
        word = strtok(NULL, " ");
        i++;
    }
}

char *predictNext(char **chain, size_t current_len) {
  char last_state[512] = "";

  createKey(last_state, chain, current_len - MARKOV_DEGREE, MARKOV_DEGREE);

  Tokens *effect = get(last_state);

  // CRITICAL: Safety check to prevent Segfault 11
  if (effect == NULL || effect->count == 0) {
    return NULL;
  }

  return effect->tok[randomIdx(effect->count)];
}

int main() {
  srand(time(NULL)); // seed with current time
  FILE *file = fopen("long.txt", "r");
  if (file == NULL) {
    printf("Error opening file\n");
    return 1;
  }

  // start with an empty token list
  Tokens token_list = {0};

  char ch;
  char current_token[100];
  int current_idx = 0;
  while ((ch = fgetc(file)) != EOF) {
    if (isalpha(ch)) {
      current_token[current_idx++] = ch;
    } else if (isdigit(ch)) {
      continue;
    } else if (ispunct(ch)) {
      continue;
    } else if (isblank(ch)) {
      current_token[current_idx] = '\0';
      da_append(token_list, strdup(current_token));
      current_idx = 0;
    }
  }
  // Handle the very last token if the file didn't end in a space
  if (current_idx > 0) {
    current_token[current_idx] = '\0';
    da_append(token_list, strdup(current_token));
  }

  fclose(file);

  // [Safety check]: Ensure we have enough space in our static arrays
  if (token_list.count >= MAX_SIZE) {
    fprintf(stderr,
            "[ERROR]: Input file has %zu tokens, which exceeds the map "
            "capacity of %d.\n",
            token_list.count, MAX_SIZE);
    return 1;
  }

  // BUILD MARKOV CHAIN TRANSITION "MATRIX"
  for (size_t i = 0; i + MARKOV_DEGREE < token_list.count; ++i) {
    char state_buffer[512];
    createKey(state_buffer, token_list.tok, i, MARKOV_DEGREE);

    char *effect = token_list.tok[i + MARKOV_DEGREE];
    if (effect == NULL)
      break;
    insert(state_buffer, effect);
    // printf("[TRANSITION] [%s] -> %s\n", state_buffer, effect);
  }

  printMap();

  size_t max_words = 100;
  // now predict words
  char *chain[100] = {0};

  // ---------------GENERATION LOOP---------------
  // 1. Seed the chain with the first 'n' tokens from your training data
  initChain(chain);

  // 2. Generate new words starting from the index after the seed
  for (size_t i = MARKOV_DEGREE; i < max_words; i++) {
    char *next_word = predictNext(chain, i);

    if (next_word == NULL) {
      printf("\n[Chain ended: No further transitions found]\n");
      break;
    }
    chain[i] = next_word;
  }

  // print the generated chain !
  printChain(chain, max_words);

  return 0;
}
