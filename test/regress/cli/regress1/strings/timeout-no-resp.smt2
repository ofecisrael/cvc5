(set-logic SLIA)
(set-info :status sat)

(declare-const x String)
(declare-const y String)
(assert (not (= (str.replace "A" (str.replace x "A" y) x) (str.replace "A" x (str.replace x y x)))))
(check-sat)