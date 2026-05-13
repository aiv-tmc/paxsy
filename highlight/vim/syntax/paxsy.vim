if exists("b:current_syntax")
    finish
endif

" Keywords
syn keyword pxKeyword   if else elif
syn keyword pxKeyword   do break continue
syn keyword pxKeyword   nop signal halt kill asm
syn keyword pxKeyword   jump return
syn keyword pxKeyword   sizeof typeof
syn keyword pxKeyword   realloc alloc free calloc memset memcpy memcmp

" DataType
syn region  pxString    start=+"+ end=+"+ skip=+\\"+    contains=@Spell
syn region  pxString    start=+'+ end=+'+ skip=+\\'+    contains=@Spell
syn match   pxNumber    "\<\d\(\d\|[a-zA-Z_.]\)*\>"
syn keyword pxNumber    none zero
syn keyword pxNumber    true false

" State
syn keyword pxState     def del pro

" Type
syn keyword pxType      Int Real Char Void Auto Struct Union
syn keyword pxType      uInt uReal uChar uVoid uAuto
syn keyword pxType      unsigned signed volatile const fixed extern static regis inline
syn keyword pxType      Byte Short Rune Word Long Float Double Noi80 Decimal Size Time Bool Str

" Logical operators
syn keyword pxLogOper   or and

" Operators
syn match   pxOperator  "[-+*%/@&$|^~=<>!?:]"
syn match   pxOperator  "[{}()[];,.]"

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
hi def link pxState         Type
hi def link pxType          Type
hi def link pxKeyword       Keyword
hi def link pxLogOper       Operator
hi def link pxOperator      Operator
hi def link pxPreProc       PreProc

let b:current_syntax = "paxsy"
