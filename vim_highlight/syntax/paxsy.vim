if exists("b:current_syntax")
    finish
endif

" Operators
syn match   pxOperator   "[-+*%/&|^~=<>!?:]"
syn match   pxOperator   "[{}();,.]"

" Logical operators
syn keyword pxLogOper   or and some

" Keywords
syn keyword pxKeyword   if else alias nop halt jump free sizeof parseof typeof alloc inter signal realloc push pop return
syn keyword pxKeyword   while do break continue

" Collection
syn match   pxFunction  "\<[a-zA-Z_][a-zA-Z0-9_]*\s*(\@="
" syn match   pxPointer   "@@\=[a-zA-Z_][a-zA-Z0-9_]*"
" syn match   pxReference "&&\=[a-zA-Z_][a-zA-Z0-9_]*"
" syn match   pxRegister  "%[a-zA-Z_][a-zA-Z0-9_]*"

" DataType
syn region  pxString    start=+"+ end=+"+ skip=+\\"+    contains=@Spell
syn region  pxString    start=+'+ end=+'+ skip=+\\'+    contains=@Spell
syn match   pxNumber    "\<\d\(\d\|[a-zA-Z_.]\)*\>"
syn keyword pxNumber    none null
" syn keyword pxNumber    true false

" Type
syn match   pxType  "\<\d\+\(Int\|Real\|Char\|Void\)\>"
syn keyword pxType  Int Real Char Void
syn keyword pxType  const unsigned signed extern static volatile public protected private
" syn keyword pxType  Byte Short Rune Word Long Float Double Noi80 Decimal Size Time Bool Str VLS
" syn keyword pxType dynamic

" State
syn keyword pxState func var let obj struct class

" Comments
syn match   pxComment "\/\/.*$"                         contains=pxTodo,@Spell
syn region  pxComment start="/\*" end="\*/"               contains=pxTodo,@Spell fold

syn keyword pxTodo                                      contained TODO FIXME NOTE XXX HACK

" Preprocessor Directives
syn match   pxPreProc "#.*$"                            contains=pxString,pxNumber,pxOperator,pxLogOper,pxType,pxKeyword,pxComment,pxTodo

" Markup
hi def link pxComment       Comment
hi def link pxTodo          Todo
hi def link pxString        String
hi def link pxNumber        Number
hi def link pxVariable      Identifier
hi def link pxPointer       Identifier
hi def link pxReference     Identifier
hi def link pxRegister      Identifier
hi def link pxFunction      Function
hi def link pxNamespace     Function
hi def link pxLabel         Label
hi def link pxType          Type
hi def link pxState         Type
hi def link pxOperator      Operator
hi def link pxLogOper       Operator
hi def link pxKeyword       Keyword
hi def link pxPreProc       PreProc

" Highlight (comment to activate your own color scheme)
" highlight pxComment         ctermfg=DarkGrey
" highlight pxTodo            ctermfg=Black ctermbg=Yellow
" highlight pxString          cterm=bold  ctermfg=LightBlue
" highlight pxNumber          cterm=bold  ctermfg=LightBlue
" highlight pxPointer         ctermfg=LightGreen
" highlight pxReference       ctermfg=LightMagenta
" highlight pxRegister        ctermfg=LightRed
" highlight pxFunction        ctermfg=LightCyan
" highlight pxType            cterm=bold  ctermfg=LightMagenta
" highlight pxState           cterm=bold  ctermfg=LightCyan
" highlight pxOperator        ctermfg=LightYellow
" highlight pxLogOper         ctermfg=Blue
" highlight pxKeyword         ctermfg=White
" highlight pxPreProc         ctermfg=LightRed

let b:current_syntax = "paxsy"
