// Derived rows built after extraction. v_conn_arc owns the terminal-overlap
// geometry used by the compatibility views; this phase turns each traversable
// direction into a physical arc and normalizes whole exact ranges to explicit
// flattened bounds required by v_trace_edge's exact mapping contract.
R"SQL(
INSERT INTO conn_arc(
    id, src_net_id, dst_net_id, src_lo, src_hi, dst_lo, dst_hi,
    map_kind, conn_id, term_map_id)
WITH directed AS (
    SELECT
        a.conn_id, a.term_map_id, 0 AS direction_order,
        a.outer_net_id AS src_net_id, a.inner_net_id AS dst_net_id,
        a.outer_lo AS src_lo, a.outer_hi AS src_hi,
        a.outer_exact AS src_exact,
        a.inner_lo AS dst_lo, a.inner_hi AS dst_hi,
        a.inner_exact AS dst_exact,
        a.map_exact
    FROM v_conn_arc a
    WHERE a.outer_net_id IS NOT NULL
      AND a.direction IN ('input', 'inout', 'ref')

    UNION ALL

    SELECT
        a.conn_id, a.term_map_id, 1 AS direction_order,
        a.inner_net_id AS src_net_id, a.outer_net_id AS dst_net_id,
        a.inner_lo AS src_lo, a.inner_hi AS src_hi,
        a.inner_exact AS src_exact,
        a.outer_lo AS dst_lo, a.outer_hi AS dst_hi,
        a.outer_exact AS dst_exact,
        a.map_exact
    FROM v_conn_arc a
    WHERE a.outer_net_id IS NOT NULL
      AND a.direction IN ('output', 'inout', 'ref')
      AND a.conn_kind IN ('signal', 'external_reference')
), normalized AS (
    SELECT
        d.conn_id,
        d.term_map_id,
        d.direction_order,
        d.src_net_id,
        d.dst_net_id,
        d.src_exact,
        d.dst_exact,
        d.map_exact,
        CASE WHEN d.src_lo IS NOT NULL THEN d.src_lo
             WHEN d.src_exact = 1 AND sn.width > 0 THEN 0 END AS norm_src_lo,
        CASE WHEN d.src_hi IS NOT NULL THEN d.src_hi
             WHEN d.src_exact = 1 AND sn.width > 0 THEN sn.width - 1 END AS norm_src_hi,
        CASE WHEN d.dst_lo IS NOT NULL THEN d.dst_lo
             WHEN d.dst_exact = 1 AND dn.width > 0 THEN 0 END AS norm_dst_lo,
        CASE WHEN d.dst_hi IS NOT NULL THEN d.dst_hi
             WHEN d.dst_exact = 1 AND dn.width > 0 THEN dn.width - 1 END AS norm_dst_hi
    FROM directed d
    JOIN net sn ON sn.id = d.src_net_id
    JOIN net dn ON dn.id = d.dst_net_id
)
SELECT
    ROW_NUMBER() OVER (
        ORDER BY conn_id, term_map_id, direction_order),
    src_net_id, dst_net_id,
    norm_src_lo, norm_src_hi, norm_dst_lo, norm_dst_hi,
    CASE WHEN map_exact = 1 AND src_exact = 1 AND dst_exact = 1
                   AND norm_src_lo IS NOT NULL AND norm_src_hi IS NOT NULL
                   AND norm_dst_lo IS NOT NULL AND norm_dst_hi IS NOT NULL
                   AND norm_src_hi - norm_src_lo = norm_dst_hi - norm_dst_lo
         THEN 'exact' ELSE 'inexact' END,
    conn_id, term_map_id
FROM normalized
ORDER BY conn_id, term_map_id, direction_order;
)SQL"
