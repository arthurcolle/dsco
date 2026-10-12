;;; .dir-locals.el --- dsco-cli project-local Emacs settings -*- no-byte-compile: t; -*-

;; Loaded automatically when Emacs visits files under this project.
;; Indentation matches .clang-format (IndentWidth: 4) and the real source
;; style (4 spaces). `compile' / `SPC p c' runs make.

((nil . ((compile-command . "make -k")
         (fill-column . 100)))
 (c-base-mode . ((indent-tabs-mode . nil)
                 (c-basic-offset . 4)))
 (c-mode . ((indent-tabs-mode . nil)
            (c-basic-offset . 4)))
 (c-ts-mode . ((indent-tabs-mode . nil)
               (c-basic-offset . 4)))
 (objc-mode . ((indent-tabs-mode . nil)
               (c-basic-offset . 4)))




)

;;; .dir-locals.el ends here
