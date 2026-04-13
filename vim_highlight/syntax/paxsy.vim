if exists("b:current_syntax")
    finish
endif

" Operators
syn match   pxOperator  "[-+*%/&|^~=<>!?:]"
syn match   pxOperator  "[{}();,.]"

" Logical operators
syn keyword pxLogOper   or and

" Keywords
syn keyword pxKeyword   if else elif
syn keyword pxKeyword   do break continue
syn keyword pxKeyword   try catch
syn keyword pxKeyword   nop
syn keyword pxKeyword   halt interflag signal kill sti cli
syn keyword pxKeyword   jump return
syn keyword pxKeyword   sizeof typeof
syn keyword pxKeyword   realloc alloc free calloc memset memcpy memcmp
syn keyword pxKeyword   extends

" DataType
syn region  pxString    start=+"+ end=+"+ skip=+\\"+    contains=@Spell
syn region  pxString    start=+'+ end=+'+ skip=+\\'+    contains=@Spell
syn match   pxNumber    "\<\d\(\d\|[a-zA-Z_.]\)*\>"
syn keyword pxNumber    none null zero error
syn keyword pxNumber    true false

" Type
"syn match   pxType  "\<\d\+\(Int\|Real\|Char\|Void\|Type\)\>"
syn keyword pxType  Int Real Char Void Type
syn keyword pxType  const unsigned signed extern static volatile regis
syn keyword pxType  Byte Short Rune Word Long Float Double Noi80 Decimal Size Time Bool Str VLS Nil

" Comments
syn match   pxComment   "\/\/.*$"                       contains=pxTodo,@Spell
syn region  pxComment   start="\/\*" end="\*\/"         contains=pxTodo,@Spell fold

syn keyword pxTodo                                      contained TODO FIXME NOTE

" Preprocessor Directives
syn match   pxPreProc   "#.*$"                          contains=pxString,pxNumber,pxOperator,pxLogOper,pxType,pxComment,pxTodo

" Markup
hi def link pxComment       Comment
hi def link pxTodo          Todo
hi def link pxString        String
hi def link pxNumber        Number
hi def link pxVariable      Identifier
hi def link pxPointer       Identifier
hi def link pxReference     Identifier
hi def link pxFunction      Function
hi def link pxType          Type
hi def link pxOperator      Operator
hi def link pxLogOper       Operator
hi def link pxKeyword       Keyword
hi def link pxPreProc       PreProc

let b:current_syntax = "paxsy"
