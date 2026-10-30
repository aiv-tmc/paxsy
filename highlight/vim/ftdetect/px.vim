" Vim filetype detection for the Paxsy language.
" Any buffer created from or read from a .px file gets filetype "px",
" which loads syntax/px.vim from a runtime directory.

augroup paxsy_ftdetect
    autocmd!
    autocmd BufRead,BufNewFile *.px setfiletype px
augroup END
