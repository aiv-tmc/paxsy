" Vim syntax file for the Paxsy language.
" Highlights every token category of the language: keywords (split into
" control flow, declarations and memory/error handling), builtin types,
" comptime aggregate keywords, booleans, line and block comments,
" double-quoted strings with {interpolation} and escapes, char literals,
" integer literals in bases 2/8/10/16 with '_' separators and U/f
" suffixes, and the full operator set.
"
" The file only defines buffer-local syntax items and links them to the
" standard highlight groups, so it works with every colorscheme.

if exists("b:current_syntax")
    finish
endif

syntax case match

" Control flow and routine management.
syntax keyword pxControl if else while do for match case break continue
syntax keyword pxControl return defer select on close routine async await

" Declaration keywords, module system and directives.
syntax keyword pxDecl let func comptime module use chan asm volatile

" Error handling and the deliberate-empty-statement keyword.
syntax keyword pxError try catch error pass

" Heap operators.
syntax keyword pxMemory alloc free

" Aggregate type definition keywords (capitalized in Paxsy).
syntax keyword pxTypeDef Struct Union Enum

" Boolean literals.
syntax keyword pxBoolean true false

" Builtin scalar and family types.
syntax keyword pxType Int8 Int16 Int32 Int64 Int128 Int256
syntax keyword pxType UInt8 UInt16 UInt32 UInt64 UInt128 UInt256
syntax keyword pxType Real32 Real64 UTF8 UTF16 UTF32
syntax keyword pxType Bool Void Auto Int UInt Real Char

" Comments: // to end of line and /* ... */ blocks (not nested).
syntax match  pxCommentLine  "//.*$"
syntax region pxCommentBlock start="/\*" end="\*/"

" Double-quoted string with escape sequences and {expression}
" interpolation; the interpolation body re-enables all highlighting.
syntax region pxString start=+"+ skip=+\\"+ end=+"+
    \ contains=pxEscape,pxInterpolation
syntax match  pxEscape +\\.+ contained
syntax region pxInterpolation matchgroup=pxInterpDelim start=+{+ end=+}+
    \ contained contains=TOP

" Char literal: one code unit or an escape (multi-byte units allowed).
syntax match pxChar +'\([^'\\]\|\\.\)\+'+

" Integer literals. The general decimal pattern is defined first so
" that the base-prefixed patterns, defined later, win at the shared
" leading '0' (in Vim the item defined last has the higher priority).
syntax match pxNumber "\v<[0-9][0-9_]*(\.[0-9_]+)?([eEpP][+-]?[0-9_]+)?[UufF]?"
syntax match pxNumber "\v<0[xX][0-9A-Fa-f_]+"
syntax match pxNumber "\v<0[bB][01_]+"
syntax match pxNumber "\v<0[oO][0-7_]+"

" Operators, longest alternatives first. Comments and strings are
" defined earlier in this file and therefore take priority over the
" operator characters they may contain (/, *, " and so on).
syntax match pxOperator "<<=\|>>>=\|>>=\|<<<\|>>>\|<<\|>>"
syntax match pxOperator "<-\|::\|\.\."
syntax match pxOperator "<=\|>=\|==\|!="
syntax match pxOperator "+=\|-=\|\*=\|/=\|%="
syntax match pxOperator "&=\|\$=\|\^=\||="
syntax match pxOperator "[-+*/%&|^~@$!:.<>=]"
syntax match pxOperator "\."

" Default colour links to the standard groups.
highlight default link pxControl     Conditional
highlight default link pxDecl        Keyword
highlight default link pxError       Exception
highlight default link pxMemory      Operator
highlight default link pxTypeDef     Structure
highlight default link pxBoolean     Boolean
highlight default link pxType        Type
highlight default link pxCommentLine Comment
highlight default link pxCommentBlock Comment
highlight default link pxString      String
highlight default link pxEscape      SpecialChar
highlight default link pxInterpDelim Delimiter
highlight default link pxChar        Character
highlight default link pxNumber      Number
highlight default link pxOperator    Operator

let b:current_syntax = "px"
