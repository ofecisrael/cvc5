(set-logic QF_SLIA)

(set-info :status sat)
(declare-fun z () (Seq Int))
(declare-fun w () (Seq Int))
(declare-fun i () Int)
(assert (> i 777))
(assert (not (= (seq.replace z (seq.unit i) w) z)))
(check-sat)
