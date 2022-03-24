(set-option :incremental false)
(set-info :status unsat)
(set-logic QF_BV)
(declare-fun v0 () (_ BitVec 4))
(declare-fun v1 () (_ BitVec 10))
(declare-fun v2 () (_ BitVec 15))
(check-sat-assuming ( (let ((_let_0 (bvneg (_ bv1 3)))) (let ((_let_1 (bvudiv ((_ zero_extend 6) v0) v1))) (let ((_let_2 ((_ extract 9 4) v1))) (let ((_let_3 (bvor _let_1 ((_ zero_extend 9) (ite (bvsgt ((_ sign_extend 7) (_ bv1 3)) v1) (_ bv1 1) (_ bv0 1)))))) (let ((_let_4 ((_ zero_extend 7) _let_0))) (let ((_let_5 (bvxor _let_3 _let_4))) (let ((_let_6 (bvneg ((_ rotate_right 0) (ite (bvsgt ((_ sign_extend 7) (_ bv1 3)) v1) (_ bv1 1) (_ bv0 1)))))) (let ((_let_7 (ite (bvsle ((_ sign_extend 7) _let_0) v1) (_ bv1 1) (_ bv0 1)))) (let ((_let_8 ((_ zero_extend 12) _let_0))) (let ((_let_9 ((_ sign_extend 14) _let_6))) (let ((_let_10 ((_ sign_extend 6) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))))) (let ((_let_11 ((_ sign_extend 9) _let_6))) (let ((_let_12 (xor (bvslt v2 ((_ zero_extend 14) ((_ rotate_right 0) (ite (bvsgt ((_ sign_extend 7) (_ bv1 3)) v1) (_ bv1 1) (_ bv0 1))))) (=> (=> (= (ite (bvule _let_9 (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2)) (bvult _let_3 _let_3) (= v2 _let_9)) (ite (bvule _let_9 (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2)) (bvult _let_3 _let_3) (= v2 _let_9))) (=> (xor (bvsgt _let_1 ((_ zero_extend 6) v0)) (bvsgt ((_ zero_extend 4) _let_2) _let_5)) (bvsge ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0)) ((_ zero_extend 3) (ite (bvsgt ((_ sign_extend 7) (_ bv1 3)) v1) (_ bv1 1) (_ bv0 1)))))) (ite (and (bvslt v2 ((_ sign_extend 14) ((_ rotate_right 0) (ite (bvsgt ((_ sign_extend 7) (_ bv1 3)) v1) (_ bv1 1) (_ bv0 1))))) (bvuge ((_ sign_extend 2) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) _let_2)) (or (bvugt _let_8 (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2)) (xor (= (bvult (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2) ((_ zero_extend 14) (ite (bvsgt ((_ sign_extend 7) (_ bv1 3)) v1) (_ bv1 1) (_ bv0 1)))) (bvugt ((_ sign_extend 9) _let_2) v2)) (xor (ite (= _let_1 ((_ sign_extend 9) ((_ rotate_right 0) (ite (bvsgt ((_ sign_extend 7) (_ bv1 3)) v1) (_ bv1 1) (_ bv0 1))))) (bvslt _let_10 _let_1) (or (bvult v2 ((_ zero_extend 11) v0)) (not (and (bvuge _let_8 v2) (bvule v0 ((_ zero_extend 3) _let_7)))))) (bvule _let_2 ((_ sign_extend 2) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0)))))) (and (not (xor (and (and (and (bvsge _let_4 v1) (bvsgt v1 ((_ sign_extend 7) (_ bv1 3)))) (bvsgt _let_3 v1)) (bvult _let_11 _let_3)) (bvslt ((_ zero_extend 2) _let_7) _let_0))) (not (xor (bvule _let_10 _let_5) (= (bvult ((_ sign_extend 5) _let_6) _let_2) (bvsgt _let_1 _let_11)))))))))) (let ((_let_13 (or (xor (= (not (not (bvult (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2) (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2)))) (not (not (bvult (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2) (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2))))) (= (not (not (bvult (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2) (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2)))) (not (not (bvult (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2) (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2)))))) (xor (= (not (not (bvult (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2) (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2)))) (not (not (bvult (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2) (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2))))) (= (not (not (bvult (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2) (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2)))) (not (not (bvult (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2) (bvand ((_ zero_extend 11) ((_ extract 3 0) (bvmul ((_ zero_extend 1) (_ bv1 3)) v0))) v2))))))))) (and (and (and _let_12 _let_12) (xor _let_13 _let_13)) (not (= v1 (_ bv0 10)))))))))))))))))) ))