#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char **tok;
    size_t count;
    size_t capacity;
} Tokens;

#define MARKOV_DEGREE 2

#define da_append(list,value)\
do {\
    if ((list).count >= ((list)).capacity) {\
      if ((list).capacity == 0) {\
        (list).capacity = 256;\
      } else {\
         (list).capacity *= 2;\
      }\
      (list).tok = realloc((list).tok,\
                               (list).capacity * sizeof(*(list).tok));\
    }\
    (list).tok[(list).count++] = (value);\
} while(0);\


#define MAX_SIZE 1024

size_t size = 0; // Current number of elements in the map
char keys[MAX_SIZE][100]; // Array to store the keys
Tokens tokens[MAX_SIZE]; // Array to store the tokens
char values[MAX_SIZE][100]; // Array to store the values

// Function to get the index of key in the keys array
int getIndex(char key[])
{
    for (size_t i = 0; i < size; i++) {
        if (strcmp(keys[i], key) == 0) {
            return i;
        }
    }
    return -1; // Key not found
}

// Function to insert a key-value pair into the map
void insert(char key[], char value[])
{
    int index = getIndex(key);
    if (index == -1) { // Key not found
        strcpy(keys[size], key);
        da_append(tokens[size], value);
        // strcpy(values[size], value);
        size++;
    }
    else { // Key found
        da_append(tokens[index], value);
        // strcpy(values[index], value);
    }
}

// Function to get the value of a key in the map
Tokens* get(char key[])
{
    int index = getIndex(key);
    printf("got index %d\n", index);
    if (index == -1) { // Key not found
        return NULL;
    }
    else { // Key found
        return &tokens[index];
        // return values[index];
    }
}

// Function to print the map
void printMap()
{
    for (size_t i = 0; i < size; i++) {
        printf("[KEY, VALUES] : [%s] -> [", keys[i]);
        for (size_t j = 0; j < tokens[i].count; j++) {
            printf(" %s ", tokens[i].tok[j]);
        }
        printf("]\n");
    }
}

// function that chooses a random index in an array.
// needs as input the array of strings and the array size in order to cap the random value
char* pickRandom(char** arr, size_t arr_size)
{
    srand(time(NULL));  // seed with current time
    if (arr_size == 0) return NULL;
    size_t idx = rand() % arr_size;
    return arr[idx];
}

// Theoretically, this should accept the transition matrix.
// We have set it as a global variable (MAP)
void predictNext(char** chain, size_t chain_length)
{
    printf("chain: %s\n", *chain);
    printf("chain_length: %zu\n", chain_length);
    // char* last_state = chain[chain_length - 1];
    int start_index = chain_length - MARKOV_DEGREE;
    if(start_index < 0) start_index = 0;
    char last_state[512] = "";
    printf("start_index: %d\n", start_index);

    for (int i = 0; i < MARKOV_DEGREE; i++) {
        strcat(last_state, chain[start_index + i]);
        if (i < MARKOV_DEGREE - 1) strcat(last_state, " ");
    }
    printf("last state: %s\n", last_state);
    Tokens* effect = get(last_state);
    chain[chain_length] = pickRandom(effect->tok, effect->count);
    printf("Predicted effect: %s\n", chain[chain_length]);
}

void create_key(char *buffer, char **source, int start_index, int degree) {
    buffer[0] = '\0';
    for (int i = 0; i < degree; i++) {
        strcat(buffer, source[start_index + i]);
        if (i < degree - 1) {
            strcat(buffer, " "); // Use a space consistently
        }
    }
}

int main() {
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

  fclose(file);


  // BUILD MARKOV CHAIN TRANSITION MATRIX
  for (size_t i = 0; i < token_list.count; ++i) {
    if(i+MARKOV_DEGREE>token_list.count){
        continue;
    }

    char state_buffer[512] = "";
    // char key[MAX_SIZE] = "";
    for (int j = 0; j < MARKOV_DEGREE; j++) {
        char *event = token_list.tok[i + j];
        // printf("[--EVENT--] %s\n", event);
        // Append the word to the buffer
        strcat(state_buffer, event);
        // Add a separator (e.g., a space) if it's not the last word in the context
        if (j < MARKOV_DEGREE - 1) {
            strcat(state_buffer, " ");
        }
    }

    // i + markov degree evaluates dynamically based on the lookahead param
    char *effect = token_list.tok[i + MARKOV_DEGREE];

    if(effect==NULL){
        effect = "\0";
    }

    insert(state_buffer, effect);
    printf("[TRANSITION] [%s] -> %s\n", state_buffer, effect);
  }

  printMap();

  size_t max_words = 100;
  // now predict words
  char* chain[100] = {0};
  chain[0] = token_list.tok[0];
  for(int i=1; i<max_words; i++){
      predictNext(chain, i);
  }

  for (size_t i=0; i<max_words; i++) {
      printf("%s ",  chain[i]);
  }


  return 0;
}
