;;; Directory Local Variables
;;; For more information see (info "(emacs) Directory Variables")
(
 (nil
  ;; for (non-)indenting braces after "if", "for" etc.:
  (c-file-style . "bsd")

  (c-basic-offset . 4)

  )

 (c++-mode
  ;; Don't mix tabs/spaces (e.g., when indenting to pos 9):
  (indent-tabs-mode . nil)
  )

 ;; To change the mode in .h headers:
 (c-mode
  (mode . c++)
  (indent-tabs-mode . nil)
  )

 )
