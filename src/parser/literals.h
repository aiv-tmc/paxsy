#ifndef LITERALS_H
#define LITERALS_H

#include "../lexer/lexer.h"
#include <stdint.h>
#include <stdbool.h>

Token parse_number_literal(Lexer* lexer);
Token parse_char_literal(Lexer* lexer);
Token parse_string_literal(Lexer* lexer);

#endif
