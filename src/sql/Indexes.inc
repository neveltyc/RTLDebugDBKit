// UNIQUE constraints already index (stmt_id, ordinal) on the three statement
// child tables and (term_id, ordinal) on net_conn; term_map's primary key
// covers (term_id, ordinal). What is added here is the other direction of
// each relation and the point queries the views document: driver = by
// target, load = by source, provenance = by each reference id.
R"SQL(
CREATE INDEX tree_node_by_parent    ON tree_node(parent_node_id, ordinal);
-- The documented access path: resolving `a.b[0].c` is one lookup per
-- segment against (parent, name). Without this the by-parent index seeks
-- the parent and then scans its children for the name -- invisible on a
-- deep tree, linear per segment on a flat one with thousands of siblings.
CREATE INDEX tree_node_by_name      ON tree_node(parent_node_id, name);
CREATE INDEX inst_by_parent         ON inst(parent_inst_id);
CREATE INDEX inst_by_module         ON inst(module_id);
CREATE INDEX inst_param_by_name     ON inst_param(name, value);
CREATE INDEX prim_by_inst      ON prim(inst_id);
CREATE INDEX net_by_inst            ON net(inst_id, name);
CREATE INDEX net_by_scope           ON net(scope_node_id, name);
-- "Where is wr_en" -- name first, instance unknown -- is the opening query
-- of a debug session, and the two instance-first indexes cannot serve it:
-- without these it is a full scan of the largest object table per lookup.
CREATE INDEX net_by_name            ON net(name);
CREATE INDEX term_by_name           ON term(name);
CREATE INDEX term_by_inst           ON term(inst_id, ordinal);
CREATE INDEX term_by_inst_name      ON term(inst_id, name);
CREATE INDEX term_map_by_net        ON term_map(inner_net_id);
CREATE INDEX net_conn_by_net        ON net_conn(outer_net_id);
CREATE INDEX conn_arc_by_src        ON conn_arc(src_net_id);
CREATE INDEX conn_arc_by_dst        ON conn_arc(dst_net_id);
-- The reverse of hier_ref's advertised back-pointer. Without it, "what does
-- this net drive across the boundary" is an indexed seek for a locally
-- named connection and a full net_conn scan for an outward tie -- the one
-- path the resolved-reference arcs actually take.
CREATE INDEX net_conn_by_href       ON net_conn(outer_hier_ref_id)
    WHERE outer_hier_ref_id IS NOT NULL;
CREATE INDEX proc_by_inst      ON proc(inst_id, ordinal);
CREATE INDEX call_site_by_inst      ON call_site(inst_id);
CREATE INDEX call_site_by_parent    ON call_site(parent_call_site_id)
    WHERE parent_call_site_id IS NOT NULL;
CREATE INDEX branch_by_inst         ON branch(inst_id);
-- The chain walk: enumerating a statement's conditions climbs
-- parent_branch_id, and finding the statements under one level descends it.
CREATE INDEX branch_by_parent       ON branch(parent_branch_id)
    WHERE parent_branch_id IS NOT NULL;
-- One arm per position under one case point, which is also the seek for
-- "the arms before this one" -- the priority a plain case carries.
-- branch_label needs none: its UNIQUE(branch_id, ordinal) already indexes
-- that pair.
CREATE UNIQUE INDEX branch_arm_by_point ON branch(parent_branch_id, ordinal)
    WHERE ordinal IS NOT NULL;
CREATE INDEX stmt_by_inst           ON stmt(inst_id, ordinal);
-- "Which statements does this level gate" -- the other direction of
-- stmt.branch_id, and the join a dead-code filter makes once per level.
CREATE INDEX stmt_by_branch         ON stmt(branch_id)
    WHERE branch_id IS NOT NULL;
CREATE INDEX stmt_by_proc      ON stmt(proc_id, sequence)
    WHERE proc_id IS NOT NULL;
-- The statement-side twin of net_dep_by_call_site. v15 put `call_site_id` on
-- v_stmt, v_stmt_target and v_stmt_operand precisely so "which statements
-- belong to this call" is a lookup rather than a trace; without this it is a
-- scan of stmt, once per call site, on the side a walk falls back to when a
-- dependency did not survive its sources.
CREATE INDEX stmt_by_call_site      ON stmt(call_site_id)
    WHERE call_site_id IS NOT NULL;
CREATE INDEX stmt_target_by_net   ON stmt_target(net_id);
CREATE INDEX assign_operand_by_net  ON assign_operand(net_id);
CREATE INDEX expr_ref_by_net        ON expr_ref(net_id);
-- A level's read set: what this condition reads, one seek. The
-- (branch_id, ordinal) UNIQUE covers the other direction.
CREATE INDEX branch_ref_by_net      ON branch_ref(net_id);

-- The closure's fan-out direction: "everything this level controls,
-- transitively". The table's primary key serves the fan-in direction, and on
-- a clustered key the other end is a full scan -- 283 us against 13 us per
-- level on VeeRwolf, for a covering index over two integers.
CREATE INDEX branch_ancestor_by_ancestor
    ON branch_ancestor(ancestor_branch_id, branch_id);
CREATE INDEX proc_event_by_proc ON proc_event(proc_id);
CREATE INDEX proc_event_by_net      ON proc_event(net_id)
    WHERE net_id IS NOT NULL;
CREATE INDEX net_dep_by_src      ON net_dep(src_net_id);
CREATE INDEX net_dep_by_tgt      ON net_dep(tgt_net_id);
CREATE INDEX net_dep_by_stmt        ON net_dep(stmt_id)
    WHERE stmt_id IS NOT NULL;
CREATE INDEX net_dep_by_call_site   ON net_dep(call_site_id)
    WHERE call_site_id IS NOT NULL;
CREATE INDEX net_dep_by_operand     ON net_dep(assign_operand_id)
    WHERE assign_operand_id IS NOT NULL;
CREATE INDEX net_dep_by_tgt_ref  ON net_dep(stmt_target_id)
    WHERE stmt_target_id IS NOT NULL;
CREATE INDEX net_dep_by_expr_ref    ON net_dep(expr_ref_id)
    WHERE expr_ref_id IS NOT NULL;
CREATE INDEX net_dep_by_primitive   ON net_dep(prim_id)
    WHERE prim_id IS NOT NULL;
CREATE INDEX net_dep_by_src_href ON net_dep(src_hier_ref_id)
    WHERE src_hier_ref_id IS NOT NULL;
CREATE INDEX net_dep_by_tgt_href ON net_dep(tgt_hier_ref_id)
    WHERE tgt_hier_ref_id IS NOT NULL;
CREATE INDEX hier_ref_by_inst       ON hier_ref(inst_id);
CREATE INDEX hier_ref_by_stmt       ON hier_ref(stmt_id)
    WHERE stmt_id IS NOT NULL;
CREATE INDEX hier_ref_by_net        ON hier_ref(resolved_net_id)
    WHERE resolved_net_id IS NOT NULL;
)SQL"
