#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char **tok;
    size_t count;
    size_t capacity;
} Tokens;

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
  while ((ch = fgetc(file)) != EOF){
      if(isalpha(ch)){
          printf("read letter: %c\n", ch);
          current_token[current_idx++] = ch;
      }
      else if(isdigit(ch)){
          printf("read digit: %c\n", ch);
          current_token[current_idx++] = ch;
      }
      else if(ispunct(ch)){
          printf("read punctuation: %c\n", ch);
      }
      else if(isblank(ch)){
          printf("token parsing completed\n");
          current_token[current_idx] = '\0';

          // TOKEN END DETECTED - append it to token list dynamic array
          if(token_list.count >= token_list.capacity){
              if(token_list.capacity == 0){
                  token_list.capacity = 256;
              }
              else{
                  token_list.capacity *= 2;
              }
              token_list.tok = realloc(token_list.tok, token_list.capacity * sizeof(*token_list.tok));
          }
          token_list.tok[token_list.count++] = strdup(current_token);
          current_idx = 0;
      }
  }

  fclose(file);


  for(size_t i = 0; i < token_list.count; i++){
      printf("%s\n", token_list.tok[i]);
  }

  return 0;
}
