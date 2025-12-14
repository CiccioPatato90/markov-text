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

#define MARKOV_DEGREE 3
#define WINDOW_SIZE MARKOV_DEGREE + 1

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
    char context_buffer[512] = "";
    for (int j = 0; j < MARKOV_DEGREE; j++) {
        char *event = token_list.tok[i + j];
        // Append the word to the buffer
        strcat(context_buffer, event);
        // Add a separator (e.g., a space) if it's not the last word in the context
        if (j < MARKOV_DEGREE - 1) {
            strcat(context_buffer, ",");
        }
    }
    // i + markov degree evaluates dynamically based on the lookahead param
    char *effect = token_list.tok[i + MARKOV_DEGREE];

    printf("[TRANSITION] [%s] -> %s\n", context_buffer, effect);
  }

  return 0;
}
