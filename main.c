#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char **tok;
    size_t count;
    size_t capacity;
} Tokens;

#define MARKOV_DEGREE 1


#define MAX_SIZE 1024

int size = 0; // Current number of elements in the map
char keys[MAX_SIZE][100]; // Array to store the keys
// Tokens tokens[MAX_SIZE]; // Array to store the tokens
char values[MAX_SIZE][100]; // Array to store the values

// Function to get the index of a key in the keys array
int getIndex(char key[])
{
    for (int i = 0; i < size; i++) {
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
        strcpy(values[size], value);
        size++;
    }
    else { // Key found
        strcpy(values[index], value);
    }
}

// Function to get the value of a key in the map
char* get(char key[])
{
    int index = getIndex(key);
    if (index == -1) { // Key not found
        return NULL;
    }
    else { // Key found
        return values[index];
    }
}

// Function to print the map
void printMap()
{
    for (int i = 0; i < size; i++) {
        printf("%s: %s\n", keys[i], values[i]);
    }
}

int main() {
  FILE *file = fopen("db.txt", "r");
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

      // TOKEN END DETECTED
      // 1. increment current_window
      // 2. check if we are at the end of the window
      // 2.1    if so, we treat the tokens
      if (token_list.count >= token_list.capacity) {
        if (token_list.capacity == 0) {
          token_list.capacity = 256;
        } else {
          token_list.capacity *= 2;
        }
        token_list.tok = realloc(token_list.tok,
                                 token_list.capacity * sizeof(*token_list.tok));
      }
      token_list.tok[token_list.count++] = strdup(current_token);
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
            strcat(state_buffer, ",");
        }
    }

    // i + markov degree evaluates dynamically based on the lookahead param
    char *effect = token_list.tok[i + MARKOV_DEGREE];

    if(effect==NULL){
        effect = "\0";
    }

    insert(state_buffer, effect);
    // printf("[TRANSITION] [%s] -> %s\n", state_buffer, effect);
  }

  printMap();

  return 0;
}
