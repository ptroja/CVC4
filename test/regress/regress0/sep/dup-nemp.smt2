(set-logic QF_ALL_SUPPORTED)
(set-info :status unsat)
(declare-sort Loc 0)
(declare-const l Loc)
(assert (sep (not (_ emp Loc Loc)) (not (_ emp Loc Loc))))
(assert (pto l l))
(check-sat)
