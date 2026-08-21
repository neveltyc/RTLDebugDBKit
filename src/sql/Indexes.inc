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
CREATE INDEX term_by_inst           ON term(inst_id, ordinal);
CREATE INDEX term_by_inst_name      ON term(inst_id, name);
CREATE INDEX term_map_by_net        ON term_map(inner_net_id);
CREATE INDEX net_conn_by_net        ON net_conn(outer_net_id);
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
CREATE INDEX stmt_by_inst           ON stmt(inst_id, ordinal);
CREATE INDEX stmt_by_proc      ON stmt(proc_id, sequence)
    WHERE proc_id IS NOT NULL;
CREATE INDEX stmt_target_by_net   ON stmt_target(net_id);
CREATE INDEX assign_operand_by_net  ON assign_operand(net_id);
CREATE INDEX expr_ref_by_net        ON expr_ref(net_id);
CREATE INDEX proc_event_by_proc ON proc_event(proc_id);
CREATE INDEX proc_event_by_net      ON proc_event(net_id)
    WHERE net_id IS NOT NULL;
CREATE INDEX net_dep_by_src      ON net_dep(src_net_id);
CREATE INDEX net_dep_by_tgt      ON net_dep(tgt_net_id);
CREATE INDEX net_dep_by_stmt        ON net_dep(stmt_id);
CREATE INDEX net_dep_by_call_site   ON net_dep(call_site_id)
    WHERE call_site_id IS NOT NULL;
CREATE INDEX net_dep_by_operand     ON net_dep(assign_operand_id)
    WHERE assign_operand_id IS NOT NULL;
CREATE INDEX net_dep_by_tgt_ref  ON net_dep(stmt_target_id);
CREATE INDEX net_dep_by_expr_ref    ON net_dep(expr_ref_id)
    WHERE expr_ref_id IS NOT NULL;
CREATE INDEX net_dep_by_primitive   ON net_dep(prim_id)
    WHERE prim_id IS NOT NULL;
CREATE INDEX net_dep_by_src_href ON net_dep(src_hier_ref_id)
    WHERE src_hier_ref_id IS NOT NULL;
CREATE INDEX net_dep_by_tgt_href ON net_dep(tgt_hier_ref_id)
    WHERE tgt_hier_ref_id IS NOT NULL;
CREATE INDEX hier_ref_by_inst       ON hier_ref(inst_id);
CREATE INDEX hier_ref_by_stmt       ON hier_ref(stmt_id);
CREATE INDEX hier_ref_by_net        ON hier_ref(resolved_net_id)
    WHERE resolved_net_id IS NOT NULL;
)SQL"
