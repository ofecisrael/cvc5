(set-info :smt-lib-version 2.6)
(set-logic QF_S)
(set-info :status unsat)

(declare-fun var0 () String)
(assert (str.in_re var0 (re.* (re.* (str.to_re "0")))))
(assert (not (str.in_re var0 (re.union (re.+ (str.to_re "0")) (re.* (str.to_re "1"))))))
(check-sat)
