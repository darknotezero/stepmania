#include "global.h"
#include "CubicSpline.h"
#include "RageLog.h"
#include <list>
using std::list;

// Spline solving optimization:
// The tridiagonal part of the system of equations for a spline of size n is
//   the same for all splines of size n.  It's not affected by the positions
//   of the points.
// So spline solving can be split into two parts.  Part 1 solves the
//   tridiagonal and stores the result.  Part 2 takes the solved tridiagonal
//   and applies it to the positions to find the coefficients.
// Part 1 only needs to be done when the number of points changes.  So this
//   could cut solve time for the same number of points substantially.
// Further optimization is to cache the part 1 results for the last 16 spline
//   sizes solved, to reduce the cost of using lots of splines with a small
//   number of sizes.

struct SplineSolutionCache
{
	void solve_diagonals_straight(vector<float>& diagonals);
	void solve_diagonals_looped(vector<float>& diagonals);
private:
	void prep_inner(size_t last, vector<float>& out);
	bool find_in_cache(list<vector<float> >& cache, vector<float>& out);
	void add_to_cache(list<vector<float> >& cache, vector<float>& out);
	list<vector<float> > straight_diagonals;
	list<vector<float> > looped_diagonals;
};

size_t const solution_cache_limit= 16;

bool SplineSolutionCache::find_in_cache(list<vector<float> >& cache, vector<float>& out)
{
	size_t out_size= out.size();
	for(list<vector<float> >::iterator entry= cache.begin();
			entry != cache.end(); ++entry)
	{
		if(out_size == entry->size())
		{
			for(size_t i= 0; i < out_size; ++i)
			{
				out[i]= (*entry)[i];
			}
			return true;
		}
	}
	return false;
}

void SplineSolutionCache::add_to_cache(list<vector<float> >& cache, vector<float>& out)
{
	if(cache.size() >= solution_cache_limit)
	{
		cache.pop_back();
	}
	cache.push_front(out);
}

void SplineSolutionCache::prep_inner(size_t last, vector<float>& out)
{
	for(size_t i= 1; i < last; ++i)
	{
		out[i]= 4.0f;
	}
}

void SplineSolutionCache::solve_diagonals_straight(vector<float>& diagonals)
{
	if(find_in_cache(straight_diagonals, diagonals))
	{
		return;
	}
	size_t last= diagonals.size();
	diagonals[0]= 2.0f;
	prep_inner(last-1, diagonals);
	diagonals[last-1]= 2.0f;
	// Operation:  Add col[0] * -.5 to col[1] to zero [r0][c1].
	diagonals[1]-= .5f;
	for(size_t i= 1; i < last-1; ++i)
	{
		// Operation:  Add col[i] / -[ri][ci] to col[i+1] to zero [ri][ci+1].
		diagonals[i+1]-= 1.0f / diagonals[i];
	}
  // Solving finished.
	add_to_cache(straight_diagonals, diagonals);
}

void SplineSolutionCache::solve_diagonals_looped(vector<float>& diagonals)
{
	if(find_in_cache(looped_diagonals, diagonals))
	{
		return;
	}
	size_t last= diagonals.size();
	diagonals[0]= 4.0f;
	prep_inner(last, diagonals);

	size_t end= last-1;
	size_t stop= end-1;
	float cedge= 1.0f; // [ri][cl]
	float redge= 1.0f; // [rl][ci]
	// The loop stops before end because the case where [ri][cl] == [ri][ci+1]
	// needs special handling.
	for(size_t i= 0; i < stop; ++i)
	{
		float next_cedge= 0.0f; // [ri+1][ce]
		float next_redge= 0.0f; // [re][ci+1]
		// Operation:  Add col[i] / -[ri][ci] to col[i+1] to zero [ri][ci+1].
		float diag_recip= 1.0f / diagonals[i];
		diagonals[i+1]-= diag_recip;
		next_redge-= redge * diag_recip;
		// Operation:  Add row[i] / -[ri][ci] to row[i+1] to zero [ri+1][ci].
		next_cedge-= cedge * diag_recip;
		// Operation:  Add col[i] * -(cedge/[ri][ci]) to col[e] to zero cedge.
		diagonals[end]-= redge * (cedge / diagonals[i]);
		cedge= next_cedge; // Do not use cedge after this point in the loop.
		// Operation:  Add row[i] * -(redge/[ri][ci]) to row[e] to zero redge.
		redge= next_redge; // Do not use redge after this point in the loop.
	}
	// [rs][ce] is 1 - cedge, [re][cs] is 1 - redge
	// Operation:  Add col[s] * -([rs][ce] / [rs][cs]) to col[e] to zero redge.
	diagonals[end]-= redge * ((1.0f - cedge) / diagonals[stop]);
  // Solving finished.
	add_to_cache(looped_diagonals, diagonals);
}

SplineSolutionCache solution_cache;

// loop_space_difference exists to handle numbers that exist in a finite
// looped space, instead of the flat infinite space.
// To put it more concretely, loop_space_difference exists to allow a spline
// to control rotation with wrapping behavior at 0.0 and 2pi, instead of
// suddenly jerking from 2pi to 0.0. -Kyz
float loop_space_difference(float a, float b, float spatial_extent);
float loop_space_difference(float a, float b, float spatial_extent)
{
	float const plus_diff= a - (b + spatial_extent);
	float const minus_diff= a - (b - spatial_extent);
	if(abs(plus_diff) < abs(minus_diff))
	{
		return plus_diff;
	}
	return minus_diff;
}

void CubicSpline::solve_looped()
{
	if(check_minimum_size()) { return; }
	size_t last= m_points.size();
	vector<float> results(m_points.size());
	vector<float> diagonals(m_points.size());
	solution_cache.solve_diagonals_looped(diagonals);
	results[0]= 3 * loop_space_difference(
		m_points[1].a, m_points[last-1].a, m_spatial_extent);
	prep_inner(last, results);
	results[last-1]= 3 * loop_space_difference(
		m_points[0].a, m_points[last-2].a, m_spatial_extent);

	// The steps to solve the system of equations look like this:
	// | 4 1 0 0 1 | -> | 4 0 0 0 1 | -> | 4 0 0 0 0 | -> | 4 0 0 0 0 |
	// | 1 4 1 0 0 | -> | 0 d 1 0 x | -> | 0 d 1 0 x | -> | 0 d 0 0 x |
	// | 0 1 4 1 0 | -> | 0 1 4 1 0 | -> | 0 1 4 1 0 | -> | 0 0 d 1 x |
	// | 0 0 1 4 1 | -> | 0 0 1 4 1 | -> | 0 0 1 4 1 | -> | 0 0 1 4 1 |
	// | 1 0 0 1 4 | -> | 1 x 0 1 4 | -> | 0 x 0 1 q | -> | 0 x x 1 q |
	// V
	// | 4 0 0 0 0 | -> | 4 0 0 0 0 | -> | 4 0 0 0 0 | -> | 4 0 0 0 0 |
	// | 0 d 0 0 0 | -> | 0 d 0 0 0 | -> | 0 d 0 0 0 | -> | 0 d 0 0 0 |
	// | 0 0 d 1 x | -> | 0 0 d 0 x | -> | 0 0 d 0 0 | -> | 0 0 d 0 0 |
	// | 0 0 1 d 1 | -> | 0 0 0 d n | -> | 0 0 0 d n | -> | 0 0 0 d 0 |
	// | 0 0 x 1 r | -> | 0 0 x n r | -> | 0 0 0 n s | -> | 0 0 0 0 t |
	// Each time through the loop performs two of these steps, 4 operations.
	// All operations on diagonals are done by the solution cache, because the
	// diagonals come out the same for all splines of a given size.

	size_t end= last-1;
	size_t stop= end-1;
	float cedge= 1.0f; // [ri][cl]
	float redge= 1.0f; // [rl][ci]
	// The loop stops before end because the case where [ri][cl] == [ri][ci+1]
	// needs special handling.
	for(size_t i= 0; i < stop; ++i)
	{
		float next_cedge= 0.0f; // [ri+1][ce]
		float next_redge= 0.0f; // [re][ci+1]
		// Operation:  Add col[i] / -[ri][ci] to col[i+1] to zero [ri][ci+1].
		float diag_recip= 1.0f / diagonals[i];
		next_redge-= redge * diag_recip;
		// Operation:  Add row[i] / -[ri][ci] to row[i+1] to zero [ri+1][ci].
		results[i+1]-= results[i] * diag_recip;
		next_cedge-= cedge * diag_recip;
		// Operation:  Add col[i] * -(cedge/[ri][ci]) to col[e] to zero cedge.
		cedge= next_cedge; // Do not use cedge after this point in the loop.
		// Operation:  Add row[i] * -(redge/[ri][ci]) to row[e] to zero redge.
		results[end]-= results[i] * (redge / diagonals[i]);
		redge= next_redge; // Do not use redge after this point in the loop.
	}
	// [rs][ce] is 1 - cedge, [re][cs] is 1 - redge
	// Operation:  Add row[s] * -([re][cs] / [rs][cs]) to row[e] to zero redge.
	results[end]-= results[stop] * ((1.0f - redge) / diagonals[stop]);

	set_results(last, diagonals, results);
}

void CubicSpline::solve_straight()
{
	if(check_minimum_size()) { return; }
	size_t last= m_points.size();
	vector<float> results(m_points.size());
	vector<float> diagonals(m_points.size());
	solution_cache.solve_diagonals_straight(diagonals);
	results[0]= 3 * (m_points[1].a - m_points[0].a);
	prep_inner(last, results);
	results[last-1]= 3 * loop_space_difference(
		m_points[last-1].a, m_points[last-2].a, m_spatial_extent);

	// The system of equations to be solved looks like this:
	// | 2 1 0 0 | = | results[0] |
	// | 1 4 1 0 | = | results[1] |
	// | 0 1 4 1 | = | results[2] |
	// | 0 0 1 2 | = | results[3] |
	// Operations are carefully chosen to only modify the values in the
	// diagonals and the results, leaving the 1s unchanged.
	// All operations on diagonals are done by the solution cache, because the
	// diagonals come out the same for all splines of a given size.
	// Operation:  Add row[0] * -.5 to row[1] to zero [r1][c0].
	results[1]-= results[0] * .5f;
	for(size_t i= 1; i < last - 1; ++i)
	{
		// Operation:  Add row[i] / -[ri][ci] to row[i+1] to zero [ri+1][ci];
		results[i]-= results[i-1] * (1.0f / diagonals[i]);
	}
	set_results(last, diagonals, results);
}

bool CubicSpline::check_minimum_size()
{
	size_t last= m_points.size();
	if(last < 2)
	{
		m_points[0].b= m_points[0].c= m_points[0].d= 0.0f;
		return true;
	}
	if(last == 2)
	{
		m_points[0].b= loop_space_difference(
			m_points[1].a, m_points[0].a, m_spatial_extent);
		m_points[0].c= m_points[0].d= 0.0f;
		// These will be used in the looping case.
		m_points[1].b= loop_space_difference(
			m_points[0].a, m_points[1].a, m_spatial_extent);
		m_points[1].c= m_points[1].d= 0.0f;
		return true;
	}
	float a= m_points[0].a;
	bool all_points_identical= true;
	for(size_t i= 1; i < m_points.size(); ++i)
	{
		m_points[i].b= m_points[i].c= m_points[i].d= 0.0f;
		if(m_points[i].a != a) { all_points_identical= false; }
	}
	return all_points_identical;
}

void CubicSpline::prep_inner(size_t last, vector<float>& results)
{
	for(size_t i= 1; i < last - 1; ++i)
	{
		results[i]= 3 * loop_space_difference(
			m_points[i+1].a, m_points[i-1].a, m_spatial_extent);
	}
}

void CubicSpline::set_results(size_t last, vector<float>& diagonals, vector<float>& results)
{
	// No more operations left, everything not a diagonal should be zero now.
	for(size_t i= 0; i < last; ++i)
	{
		results[i]/= diagonals[i];
	}
	// Now we can go through and set the b, c, d values of each point.
	// b, c, d values of the last point are not set because they are unused.
	for(size_t i= 0; i < last; ++i)
	{
		size_t next= (i+1) % last;
		float diff= loop_space_difference(
			m_points[next].a, m_points[i].a, m_spatial_extent);
		m_points[i].b= results[i];
		m_points[i].c= (3 * diff) - (2 * results[i]) - results[next];
		m_points[i].d= (2 * -diff) + results[i] + results[next];
#define UNNAN(n) if(n != n) { n = 0.0f; }
		UNNAN(m_points[i].b);
		UNNAN(m_points[i].c);
		UNNAN(m_points[i].d);
#undef UNNAN
	}
	// Solving is now complete.
}

float CubicSpline::evaluate(float t, bool loop) const
{
	if(m_points.empty())
	{
		return 0.0f;
	}
	int flort= static_cast<int>(t);
	if(loop)
	{
		float max_t= m_points.size();
		while(t >= max_t) { t-= max_t; }
		while(t < 0.0f) { t+= max_t; }
		flort= static_cast<int>(t);
	}
	else
	{
		if(flort <= 0)
		{
			return m_points[0].a;
		}
		else if(static_cast<size_t>(flort) >= m_points.size() - 1)
		{
			return m_points[m_points.size() - 1].a;
		}
	}
	size_t p= min(static_cast<size_t>(flort), m_points.size()-1);
	float tfrac= t - static_cast<float>(flort);
	float tsq= tfrac * tfrac;
	float tcub= tsq * tfrac;
	return m_points[p].a + (m_points[p].b * tfrac) +
		(m_points[p].c * tsq) + (m_points[p].d * tcub);
}

float CubicSpline::evaluate_derivative(float t, bool loop) const
{
	if(m_points.empty())
	{
		return 0.0f;
	}
	int flort= static_cast<int>(t);
	if(loop)
	{
		float max_t= m_points.size();
		while(t >= max_t) { t-= max_t; }
		while(t < 0.0f) { t+= max_t; }
		flort= static_cast<int>(t);
	}
	else
	{
		if(static_cast<size_t>(flort) >= m_points.size() - 1)
		{
			return 0.0f;
		}
	}
	size_t p= min(static_cast<size_t>(flort), m_points.size()-1);
	float tfrac= t - static_cast<float>(flort);
	float tsq= tfrac * tfrac;
	return m_points[p].b + (2.0f * m_points[p].c * tfrac) +
		(3.0f * m_points[p].d * tsq);
}

void CubicSpline::set_point(size_t i, float v)
{
	ASSERT_M(i < m_points.size(), "CubicSpline::set_point requires the index to be less than the number of points.");
	m_points[i].a= v;
}

void CubicSpline::set_coefficients(size_t i, float b, float c, float d)
{
	ASSERT_M(i < m_points.size(), "CubicSpline: point index must be less than the number of points.");
	m_points[i].b= b;
	m_points[i].c= c;
	m_points[i].d= d;
}

void CubicSpline::get_coefficients(size_t i, float& b, float& c, float& d)
{
	ASSERT_M(i < m_points.size(), "CubicSpline: point index must be less than the number of points.");
	b= m_points[i].b;
	c= m_points[i].c;
	d= m_points[i].d;
}

void CubicSpline::resize(size_t s)
{
	m_points.resize(s);
}

size_t CubicSpline::size() const
{
	return m_points.size();
}

bool CubicSpline::empty() const
{
	return m_points.empty();
}

void CubicSplineN::solve()
{
	if(!m_dirty) { return; }
	if(loop)
	{
		for(spline_cont_t::iterator spline= m_splines.begin();
				spline != m_splines.end(); ++spline)
		{
			spline->solve_looped();
		}
	}
	else
	{
		for(spline_cont_t::iterator spline= m_splines.begin();
				spline != m_splines.end(); ++spline)
		{
			spline->solve_straight();
		}
	}
	m_dirty= false;
}

void CubicSplineN::evaluate(float t, vector<float>& v) const
{
	for(spline_cont_t::const_iterator spline= m_splines.begin();
			spline != m_splines.end(); ++spline)
	{
		v.push_back(spline->evaluate(t, loop));
	}
}

void CubicSplineN::evaluate_derivative(float t, vector<float>& v) const
{
	for(spline_cont_t::const_iterator spline= m_splines.begin();
			spline != m_splines.end(); ++spline)
	{
		v.push_back(spline->evaluate_derivative(t, loop));
	}
}

void CubicSplineN::set_point(size_t i, vector<float> const& v)
{
	ASSERT_M(v.size() == m_splines.size(), "CubicSplineN::set_point requires the passed point to be the same dimension as the spline.");
	for(size_t n= 0; n < m_splines.size(); ++n)
	{
		m_splines[n].set_point(i, v[n]);
	}
	m_dirty= true;
}

void CubicSplineN::set_coefficients(size_t i, vector<float> const& b,
	vector<float> const& c, vector<float> const& d)
{
	ASSERT_M(b.size() == c.size() && c.size() == d.size() &&
		d.size() == m_splines.size(), "CubicSplineN: coefficient vectors must be "
		"the same dimension as the spline.");
	for(size_t n= 0; n < m_splines.size(); ++n)
	{
		m_splines[n].set_coefficients(i, b[n], c[n], d[n]);
	}
	m_dirty= true;
}

void CubicSplineN::get_coefficients(size_t i, vector<float>& b,
	vector<float>& c, vector<float>& d)
{
	ASSERT_M(b.size() == c.size() && c.size() == d.size() &&
		d.size() == m_splines.size(), "CubicSplineN: coefficient vectors must be "
		"the same dimension as the spline.");
	for(size_t n= 0; n < m_splines.size(); ++n)
	{
		m_splines[n].get_coefficients(i, b[n], c[n], d[n]);
	}
}

void CubicSplineN::set_spatial_extent(size_t i, float extent)
{
	ASSERT_M(i < m_splines.size(), "CubicSplineN: index of spline to set extent"
		" of is out of range.");
	m_splines[i].m_spatial_extent= extent;
	m_dirty= true;
}

float CubicSplineN::get_spatial_extent(size_t i)
{
	ASSERT_M(i < m_splines.size(), "CubicSplineN: index of spline to get extent"
		" of is out of range.");
	return m_splines[i].m_spatial_extent;
}

void CubicSplineN::resize(size_t s)
{
	for(spline_cont_t::iterator spline= m_splines.begin();
			spline != m_splines.end(); ++spline)
	{
		spline->resize(s);
	}
	m_dirty= true;
}

size_t CubicSplineN::size() const
{
	if(!m_splines.empty())
	{
		return m_splines[0].size();
	}
	return 0;
}

bool CubicSplineN::empty() const
{
	return m_splines.empty() || m_splines[0].empty();
}

void CubicSplineN::redimension(size_t d)
{
	m_splines.resize(d);
	m_dirty= true;
}

size_t CubicSplineN::dimension() const
{
	return m_splines.size();
}

#include "LuaBinding.h"

struct LunaCubicSplineN : Luna<CubicSplineN>
{
	static size_t dimension_index(T* p, lua_State* L, int s)
	{
		size_t i= static_cast<size_t>(IArg(s)-1);
		if(i >= p->dimension())
		{
			luaL_error(L, "Spline dimension index out of range.");
		}
		return i;
	}
	static size_t point_index(T* p, lua_State* L, int s)
	{
		size_t i= static_cast<size_t>(IArg(s)-1);
		if(i >= p->size())
		{
			luaL_error(L, "Spline point index out of range.");
		}
		return i;
	}
	static int solve(T* p, lua_State* L)
	{
		p->solve();
		COMMON_RETURN_SELF;
	}
	static int evaluate(T* p, lua_State* L)
	{
		vector<float> pos;
		p->evaluate(FArg(1), pos);
		lua_createtable(L, pos.size(), 0);
		for(size_t i= 0; i < pos.size(); ++i)
		{
			lua_pushnumber(L, pos[i]);
			lua_rawseti(L, -2, i+1);
		}
		return 1;
	}
	static int evaluate_derivative(T* p, lua_State* L)
	{
		vector<float> pos;
		p->evaluate_derivative(FArg(1), pos);
		lua_createtable(L, pos.size(), 0);
		for(size_t i= 0; i < pos.size(); ++i)
		{
			lua_pushnumber(L, pos[i]);
			lua_rawseti(L, -2, i+1);
		}
		return 1;
	}
	static void get_element_table_from_stack(T* p, lua_State* L, int s,
		size_t limit, vector<float>& ret)
	{
		size_t elements= lua_objlen(L, s);
		// Too many elements is not an error because allowing it allows the user
		// to reuse the same position data set after changing the dimension size.
		// The same is true for too few elements.
		for(size_t e= 0; e < elements; ++e)
		{
			lua_rawgeti(L, s, e+1);
			ret.push_back(FArg(-1));
		}
		while(ret.size() < limit)
		{
			ret.push_back(0.0f);
		}
	}
	static void set_point_from_stack(T* p, lua_State* L, size_t i, int s)
	{
		if(!lua_istable(L, s))
		{
			luaL_error(L, "Spline point must be a table.");
		}
		vector<float> pos;
		get_element_table_from_stack(p, L, s, p->dimension(), pos);
		p->set_point(i, pos);
	}
	static int set_point(T* p, lua_State* L)
	{
		size_t i= point_index(p, L, 1);
		set_point_from_stack(p, L, i, 2);
		COMMON_RETURN_SELF;
	}
	static void set_coefficients_from_stack(T* p, lua_State* L, size_t i, int s)
	{
		if(!lua_istable(L, s) || !lua_istable(L, s+1) || !lua_istable(L, s+2))
		{
			luaL_error(L, "Spline coefficient args must be three tables.");
		}
		size_t limit= p->dimension();
		vector<float> b; get_element_table_from_stack(p, L, s, limit, b);
		vector<float> c; get_element_table_from_stack(p, L, s+1, limit, c);
		vector<float> d; get_element_table_from_stack(p, L, s+2, limit, d);
		p->set_coefficients(i, b, c, d);
	}
	static int set_coefficients(T* p, lua_State* L)
	{
		size_t i= point_index(p, L, 1);
		set_coefficients_from_stack(p, L, i, 2);
		COMMON_RETURN_SELF;
	}
	static int get_coefficients(T* p, lua_State* L)
	{
		size_t i= point_index(p, L, 1);
		size_t limit= p->dimension();
		vector<vector<float> > coeff(3);
		coeff[0].resize(limit);
		coeff[1].resize(limit);
		coeff[2].resize(limit);
		p->get_coefficients(i, coeff[0], coeff[1], coeff[2]);
		lua_createtable(L, 3, 0);
		for(size_t co= 0; co < coeff.size(); ++co)
		{
			lua_createtable(L, limit, 0);
			for(size_t v= 0; v < limit; ++v)
			{
				lua_pushnumber(L, coeff[co][v]);
				lua_rawseti(L, -2, v+1);
			}
			lua_rawseti(L, -2, co+1);
		}
		return 1;
	}
	static int set_spatial_extent(T* p, lua_State* L)
	{
		size_t i= dimension_index(p, L, 1);
		p->set_spatial_extent(i, FArg(2));
		COMMON_RETURN_SELF;
	}
	static int get_spatial_extent(T* p, lua_State* L)
	{
		size_t i= dimension_index(p, L, 1);
		lua_pushnumber(L, p->get_spatial_extent(i));
		return 1;
	}
	static int resize(T* p, lua_State* L)
	{
		int siz= IArg(1);
		if(siz < 0)
		{
			luaL_error(L, "A spline cannot have less than 0 points.");
		}
		p->resize(static_cast<size_t>(siz));
		COMMON_RETURN_SELF;
	}
	static int size(T* p, lua_State* L)
	{
		lua_pushnumber(L, p->size());
		return 1;
	}
	static int redimension(T* p, lua_State* L)
	{
		if(p->m_owned_by_actor)
		{
			luaL_error(L, "This spline cannot be redimensioned because it is "
				"owned by an actor that relies on it having fixed dimensions.");
		}
		int dim= IArg(1);
		if(dim < 0)
		{
			luaL_error(L, "A spline cannot have less than 0 dimensions.");
		}
		p->redimension(static_cast<size_t>(dim));
		COMMON_RETURN_SELF;
	}
	static int dimension(T* p, lua_State* L)
	{
		lua_pushnumber(L, p->dimension());
		return 1;
	}
	static int empty(T* p, lua_State* L)
	{
		lua_pushboolean(L, p->empty());
		return 1;
	}
	static int set_loop(T* p, lua_State* L)
	{
		p->loop= lua_toboolean(L, 1);
		COMMON_RETURN_SELF;
	}
	static int get_loop(T* p, lua_State* L)
	{
		lua_pushboolean(L, p->loop);
		return 1;
	}
	LunaCubicSplineN()
	{
		ADD_METHOD(solve);
		ADD_METHOD(evaluate);
		ADD_METHOD(evaluate_derivative);
		ADD_METHOD(set_point);
		ADD_METHOD(set_coefficients);
		ADD_METHOD(get_coefficients);
		ADD_METHOD(set_spatial_extent);
		ADD_METHOD(get_spatial_extent);
		ADD_METHOD(resize);
		ADD_METHOD(size);
		ADD_METHOD(redimension);
		ADD_METHOD(dimension);
		ADD_METHOD(empty);
		ADD_METHOD(set_loop);
		ADD_METHOD(get_loop);
	}
};
LUA_REGISTER_CLASS(CubicSplineN)

// Side note:  Actually written between 2014/12/26 and 2014/12/28
/*
 * Copyright (c) 2014-2015 Eric Reese
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
