#ifndef LITERALS_H
#define LITERALS_H

#include "../lexer/lexer.h"
#include "../errhandler/errhandler.h"
#include <stdint.h>
#include <stdbool.h>

Token literal__parse_number(Lexer* lexer);
Token literal__parse_char(Lexer* lexer);
Token literal__parse_string(Lexer* lexer);
Token literal__parse_concatenated(Lexer* lexer);

#endif
