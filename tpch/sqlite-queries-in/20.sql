select
	s.name,
	s.address
from
	supplier s,
	nation n
where
	s.suppkey in (
		select
			ps.suppkey
		from
			partsupp ps
		where
			ps.partkey in (
				select
					partkey
				from
					part
				where
					name like 'b%'
			)
			and ps.availqty > (
				select
					0.5 * sum(l.quantity)
				from
					lineitem l
				where
					l.partkey = ps.partkey
					and l.suppkey = ps.suppkey
					and l.shipdate >= date('1995-01-01')
					and l.shipdate < date('1995-01-01', '+1 year')
			)
	)
	and s.nationkey = n.nationkey
	and n.name = 'BRAZIL'
order by
	s.name;