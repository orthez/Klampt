#include "motionplanning.h"
#include <KrisLibrary/planning/AnyMotionPlanner.h>
#include <KrisLibrary/planning/ExplicitCSpace.h>
#include <KrisLibrary/planning/CSpaceHelpers.h>
#include "pyerr.h"
#include <KrisLibrary/graph/IO.h>
#include <KrisLibrary/graph/DirectedGraph.h>
#include <KrisLibrary/graph/Callback.h>
#include <KrisLibrary/structs/FixedSizeHeap.h>
#include <KrisLibrary/math/random.h>
#include <KrisLibrary/Timer.h>
#include <Python.h>
#include <iostream>
#include <fstream>
#include <exception>
#include <vector>
#include <map>
using namespace std;

void setRandomSeed(int seed)
{
  Math::Srand(seed);
}

PyObject* ToPy(int x) { return PyInt_FromLong(x); }
PyObject* ToPy(double x) { return PyFloat_FromDouble(x); }
PyObject* ToPy(const string& x) { return PyString_FromString(x.c_str()); }

template <class T>
PyObject* ToPy(const std::vector<T>& x)
{
  PyObject* ls = PyList_New(x.size());
  PyObject* pItem;
  if(ls == NULL) {
    goto fail;
  }
	
  for(Py_ssize_t i = 0; i < PySequence_Size(ls); i++) {
    pItem = ::ToPy(x[i]);
    if(pItem == NULL)
      goto fail;
    PyList_SetItem(ls, i, pItem);
  }
  
  return ls;
  
 fail:
  Py_XDECREF(ls);
  throw PyException("Failure during ToPy");
  return NULL;
}

PyObject* ToPy(const Config& x) {
  PyObject* ls = PyList_New(x.n);
  PyObject* pItem;
  if(ls == NULL) {
    goto fail;
  }
	
  for(Py_ssize_t i = 0; i < PySequence_Size(ls); i++) {
    pItem = PyFloat_FromDouble(x[(int)i]);
    if(pItem == NULL)
      goto fail;
    PyList_SetItem(ls, i, pItem);
  }
  
  return ls;
  
 fail:
  Py_XDECREF(ls);
  throw PyException("Failure during ToPy");
  return NULL;
}

PyObject* PyListFromVector(const std::vector<double>& x)
{
  PyObject* ls = PyList_New(x.size());
  PyObject* pItem;
  if(ls == NULL) {
    goto fail;
  }
	
  for(Py_ssize_t i = 0; i < PySequence_Size(ls); i++) {
    pItem = PyFloat_FromDouble(x[(int)i]);
    if(pItem == NULL)
      goto fail;
    PyList_SetItem(ls, i, pItem);
  }
  
  return ls;
  
 fail:
  Py_XDECREF(ls);
  throw PyException("Failure during PyListFromVector");
  return NULL;
}

bool PyListToVector(PyObject* seq,std::vector<double>& x)
{
  if(!PySequence_Check(seq))
    return false;
  
  x.resize(PySequence_Size(seq));
  for(Py_ssize_t i = 0; i < PySequence_Size(seq); i++)  {
    PyObject* v=PySequence_GetItem(seq, i);
    assert(v != NULL);
    x[(int)i] = PyFloat_AsDouble(v);
    Py_XDECREF(v);
    if(PyErr_Occurred()) return false;
  }
  return true;
}

PyObject* PyListFromConfig(const Config& x)
{
  PyObject* ls = PyList_New(x.n);
  PyObject* pItem;
  if(ls == NULL) {
    goto fail;
  }
	
  for(Py_ssize_t i = 0; i < PySequence_Size(ls); i++) {
    pItem = PyFloat_FromDouble(x[(int)i]);
    if(pItem == NULL)
      goto fail;
    PyList_SetItem(ls, i, pItem);
  }
  
  return ls;
  
 fail:
  Py_XDECREF(ls);
  throw PyException("Failure during PyListFromConfig");
  return NULL;
}


bool PyListToConfig(PyObject* seq,Config& x)
{
  if(!PySequence_Check(seq))
    return false;
  
  x.resize(PySequence_Size(seq));
  for(Py_ssize_t i = 0; i < PySequence_Size(seq); i++) {
    PyObject* v=PySequence_GetItem(seq, i);
    assert(v != NULL);
    x[(int)i] = PyFloat_AsDouble(v);
    Py_XDECREF(v);
    if(PyErr_Occurred()) return false;
  }
  return true;
}

struct TesterStats
{
  TesterStats();
  void reset(double cost,double probability,double count);
  void update(double testcost,bool testtrue,double strength=1.0);

  double cost;
  double probability;
  double count;
};

TesterStats::TesterStats()
:cost(0),probability(0),count(0)
{}
void TesterStats::reset(double _cost,double _probability,double _count)
{
  cost=_cost;
  probability=_probability;
  count=_count;
}
void TesterStats::update(double testcost,bool testtrue,double strength)
{
  double newcount = count+strength;
  if(newcount == 0) newcount=1;
  double oldweight = count/newcount;
  double newweight = 1.0-oldweight;
  cost = oldweight*cost + newweight*testcost;
  if(testtrue)
    probability = oldweight*probability + newweight;
  else
    probability = oldweight*probability;
  count += strength;
}

void OptimizeTestingOrder(const vector<TesterStats>& stats,vector<vector<int> >& deps,vector<int>& order)
{
  vector<pair<double,int> > priority(stats.size());
  for(size_t i=0;i<stats.size();i++) {
    priority[i].first = stats[i].cost / (1.0-stats[i].probability);
    if(IsNaN(priority[i].first)) priority[i].first = 0.0;
    priority[i].second = (int)i;
  }
  if(deps.empty()) {
    sort(priority.begin(),priority.end());
    order.resize(stats.size());
    for(size_t i=0;i<stats.size();i++) 
      order[i] = priority[i].second;
  }
  else {
    //more complicated version with dependency graph
    Graph::DirectedGraph<int,int> G;
    for(size_t i=0;i<stats.size();i++) 
      G.AddNode(int(i));
    for(size_t i=0;i<stats.size();i++) 
      for(size_t j=0;j<deps[i].size();j++)
        G.AddEdge(deps[i][j],i);
    //compute topological sort
    Graph::TopologicalSortCallback<int> callback;
    G.DFS(callback);
    if(callback.hasCycle) {
      fprintf(stderr,"motionplanning: WARNING: Test dependency order has cycles... breaking arbitrarily\n");
    }
    //revise priorities from bottom up based off of children
    //a parent node's priority is the minimum of (cost of this + cost of best child) / (1-probability of this*probability of best child)
    vector<int> bestChild(stats.size(),-1);
    vector<double> depcosts(stats.size());
    vector<double> depprobs(stats.size());
    for(size_t i=0;i<stats.size();i++) {
      depcosts[i] = stats[i].cost;
      depprobs[i] = stats[i].probability;
    }
    //reverse gives bottom up traversal
    std::reverse(callback.list.begin(),callback.list.end());
    for(list<int>::const_iterator i=callback.list.begin();i!=callback.list.end();i++) {
      if(G.OutDegree(*i) > 0) {
        //has dependencies...
        double bestPriority = Inf;
        int best = -1;
        Graph::EdgeIterator<int> e;
        for(G.Begin(*i,e);!e.end();e++) {
          int j=e.target();
          if(G.InDegree(j) > 1) //has more than one dependency
            fprintf(stderr,"motionplanning: WARNING: Constraint %d has multiple dependencies including %d, can't really optimize yet\n",j,*i);
          double priority = (depcosts[*i]+depcosts[j])/(1.0-depprobs[*i]*depprobs[j]);
          if(priority < bestPriority || best < 0) {
            best = j;
            bestPriority = priority;
          }
        }
        depcosts[*i] = depcosts[*i] + depcosts[best];
        depprobs[*i] = depprobs[*i] * depprobs[best];
        priority[*i].first = bestPriority;
      }
    }
    //now search the graph from top down, extracting the lowest priority items from fringe
    order.resize(0);
    order.reserve(stats.size());
    FixedSizeHeap<double> queue(stats.size());
    vector<bool> visited(stats.size(),false);
    for(size_t i=0;i<stats.size();i++) 
      if(G.InDegree(i) == 0) {
        queue.push(int(i),-priority[i].first);
      }
    while(true) {
      int i;
      if(!queue.empty()) {
        i=queue.top();
        queue.pop();
      }
      else {
        //should only get here with cycles... break them in arbitrary order
        i = -1;
        for(size_t j=0;j<visited.size();j++)
          if(!visited[j]) {
            i=(int)j;
            break;
          }
        if(i < 0) break; //done
      }
      visited[i] = true;
      order.push_back(i);
      //go through constraints dependent on i and put them on the queue
      Graph::EdgeIterator<int> e;
      for(G.Begin(i,e);!e.end();e++) {
        int j=e.target();
        queue.push(j,-priority[j].first);
      }
    }
  }
  /*
  //Debugging
  printf("Costs:");
  for(size_t i=0;i<stats.size();i++) 
    printf(" %g",stats[i].cost);
  printf("\n");
  printf("Probabilities:");
  for(size_t i=0;i<stats.size();i++) 
    printf(" %g",stats[i].probability);
  printf("\n");
  printf("Dependencies:");
  for(size_t i=0;i<deps.size();i++)  {
    printf(" [");
    for(size_t j=0;j<deps[i].size();j++) {
      if(j > 0) printf(",");
      printf("%d",deps[i][j]);
    }
    printf("]");
  }
  printf("\n");
  printf("Order:");
  for(size_t i=0;i<order.size();i++) 
    printf(" %d",order[i]);
  printf("\n");
  //getchar();
  */
}

class PyCSpace;
class PyEdgePlanner;

/** A CSpace that calls python routines for its functionality */
class PyCSpace : public ExplicitCSpace
{
public:
  PyCSpace()
    :sample(NULL),sampleNeighborhood(NULL),
     distance(NULL),interpolate(NULL),edgeResolution(0.001),adaptive(false),cacheq(NULL),cacheq2(NULL),cachex(NULL),cachex2(NULL)
  {}

  virtual ~PyCSpace() {
    Py_XDECREF(sample);
    Py_XDECREF(sampleNeighborhood);
    for(size_t i=0;i<feasibleTests.size();i++)
      Py_XDECREF(feasibleTests[i]);
    for(size_t i=0;i<visibleTests.size();i++)
      Py_XDECREF(visibleTests[i]);
    Py_XDECREF(distance);
    Py_XDECREF(interpolate);
    Py_XDECREF(cachex);
    Py_XDECREF(cachex2);
  }

  PyObject* UpdateTempConfig(const Config& q) {
    //PROBLEM when the values of q change, its address doesnt! we still have to re-make it
    //if(&q == cacheq) return cachex;
    Py_XDECREF(cachex);
    cacheq = &q;
    cachex = PyListFromConfig(q);
    return cachex;
  }
  PyObject* UpdateTempConfig2(const Config& q) {
    //PROBLEM when the values of q change, its address doesnt! we still have to re-make it
    //if(&q == cacheq2) return cachex2;
    Py_XDECREF(cachex2);
    cacheq2 = &q;
    cachex2 = PyListFromConfig(q);
    return cachex2;
  }

  void operator = (const PyCSpace& rhs)
  {
    sample = rhs.sample;
    sampleNeighborhood = rhs.sampleNeighborhood;
    feasibleTests = rhs.feasibleTests;
    visibleTests = rhs.visibleTests;
    feasibleStats = rhs.feasibleStats;
    visibleStats = rhs.visibleStats;
    feasibleTestOrder = rhs.feasibleTestOrder;
    visibleTestOrder = rhs.visibleTestOrder;
    feasibleTestDeps = rhs.feasibleTestDeps;
    visibleTestDeps = rhs.visibleTestDeps;
    constraintNames = rhs.constraintNames;
    constraintMap = rhs.constraintMap;
    distance = rhs.distance;
    interpolate = rhs.interpolate;
    edgeResolution = rhs.edgeResolution;
    Py_XINCREF(sample);
    Py_XINCREF(sampleNeighborhood);
    for(size_t i=0;i<feasibleTests.size();i++)
      Py_XINCREF(feasibleTests[i]);
    for(size_t i=0;i<visibleTests.size();i++)
      Py_XINCREF(visibleTests[i]);
    Py_XINCREF(distance);
    Py_XINCREF(interpolate);
  }

  virtual void Sample(Config& x) {
    if(!sample) {
      throw PyException("Python sample method not defined");
    }
    PyObject* result = PyObject_CallFunctionObjArgs(sample,NULL);
    if(!result) {
      if(!PyErr_Occurred()) {
	throw PyException("Python sample method failed");
      }
      else {
	throw PyPyErrorException();
      }
    }
    bool res=PyListToConfig(result,x);
    if(!res) {
      Py_DECREF(result);
      throw PyException("Python sample method didn't return sequence");
    }
    Py_DECREF(result);
  }

  virtual void SampleNeighborhood(const Config& c,double r,Config& x)
  {
    if(!sampleNeighborhood) {
      CSpace::SampleNeighborhood(c,r,x);
    }
    else {
      PyObject* pyc=UpdateTempConfig(c);
      PyObject* pyr=PyFloat_FromDouble(r);
      PyObject* result = PyObject_CallFunctionObjArgs(sampleNeighborhood,pyc,pyr,NULL);
      if(!result) {
	Py_DECREF(pyr);
	if(!PyErr_Occurred()) {
	  throw PyException("Python sampleneighborhood method failed");
	}
	else {
	  throw PyPyErrorException();
	}
      }
      bool res=PyListToConfig(result,x);
      if(!res) {
	Py_DECREF(pyr);
	Py_DECREF(result);
	throw PyException("Python sampleNeighborhood method did not return a list");
      }
      Py_DECREF(pyr);
      Py_DECREF(result);
    }
  }


  virtual int NumObstacles() { return feasibleTests.size(); }
  virtual std::string ObstacleName(int obstacle) {
    if(obstacle < 0 || obstacle >= (int)feasibleTests.size()) return "";
    return constraintNames[obstacle];
  }
  virtual bool IsFeasible(const Config& x,int obstacle) {
    if(obstacle < 0 || obstacle >= (int)feasibleTests.size()) return false;

    if(feasibleTests[obstacle] == NULL) {
      stringstream ss;
      ss<<"Python feasible test for constraint "<<constraintNames[obstacle]<<"not defined"<<endl;
      throw PyException(ss.str().c_str());
    }

    if(adaptive) timer.Reset();
    PyObject* pyx = UpdateTempConfig(x);
    PyObject* result = PyObject_CallFunctionObjArgs(feasibleTests[obstacle],pyx,NULL);
    if(result == NULL) {
      if(!PyErr_Occurred()) {
	throw PyException("An error occurred when calling feasible");
      }
      else {
	throw PyPyErrorException();
      }
    }
    if(!PyBool_Check(result)) {
      Py_DECREF(result);
      throw PyException("Python feasible test method didn't return bool");
    }
    bool res=(result == Py_True);
    Py_DECREF(result);
    if(adaptive)
      feasibleStats[obstacle].update(timer.ElapsedTime(),res);
    if(!res) {
      return false;
    }
    return true;
  }

  virtual bool IsFeasible(const Config& x)
  {
    if(feasibleTests.empty()) {
      throw PyException("Python feasible method not defined");
    }
    PyObject* pyx = UpdateTempConfig(x);
    for(size_t i=0;i<feasibleTests.size();i++) {
      int obstacle = (feasibleTestOrder.empty() ? (int)i : feasibleTestOrder[i]);
      if(feasibleTests[obstacle] == NULL) {
	stringstream ss;
	ss<<"Python feasible test for constraint "<<constraintNames[obstacle]<<"not defined"<<endl;
	throw PyException(ss.str().c_str());
      }

      if(adaptive) timer.Reset();
      PyObject* result = PyObject_CallFunctionObjArgs(feasibleTests[obstacle],pyx,NULL);
      if(result == NULL) {
	if(!PyErr_Occurred()) {
	  throw PyException("An error occurred when calling feasible");
	}
	else {
	  throw PyPyErrorException();
	}
      }
      if(!PyBool_Check(result)) {
	Py_DECREF(result);
	throw PyException("Python feasible test method didn't return bool");
      }
      bool res=(result == Py_True);
      Py_DECREF(result);
      if(adaptive) 
        feasibleStats[obstacle].update(timer.ElapsedTime(),res);
      if(!res) {
	return false;
      }
    }
    return true;
  }

  virtual bool IsVisible(const Config& a,const Config& b) {
    EdgePlanner* e = LocalPlanner(a,b);
    bool res = e->IsVisible();
    delete e;
    return res;
  }

  virtual bool IsVisible(const Config& a,const Config& b,int obstacle) {
    EdgePlanner* e = LocalPlanner(a,b,obstacle);
    bool res = e->IsVisible();
    delete e;
    return res;
  }

  virtual EdgePlanner* LocalPlanner(const Config& a,const Config& b,int obstacle);

  virtual EdgePlanner* LocalPlanner(const Config& a,const Config& b);

  virtual double Distance(const Config& x, const Config& y)
  {
    if(!distance) {
      return CSpace::Distance(x,y);
    }
    else {
      PyObject* pyx = UpdateTempConfig(x);
      PyObject* pyy = UpdateTempConfig2(y);
      PyObject* result = PyObject_CallFunctionObjArgs(distance,pyx,pyy,NULL);
      if(!result) {
	if(!PyErr_Occurred()) {
	  throw PyException("Python distance method failed");
	}
	else {
	  throw PyPyErrorException();
	}
      }
      if(!PyFloat_Check(result)) {
	Py_DECREF(result);
	throw PyException("Python distance didn't return float");
      }
      double res=PyFloat_AsDouble(result);
      Py_DECREF(result);
      return res;
    }
  }
  virtual void Interpolate(const Config& x,const Config& y,double u,Config& out)
  {
    if(!interpolate) {
      CSpace::Interpolate(x,y,u,out);
    }
    else {
      PyObject* pyx = UpdateTempConfig(x);
      PyObject* pyy = UpdateTempConfig2(y);
      PyObject* pyu = PyFloat_FromDouble(u);
      PyObject* result = PyObject_CallFunctionObjArgs(interpolate,pyx,pyy,pyu,NULL);
      Py_DECREF(pyu);
      if(!result) {
	if(!PyErr_Occurred()) {
	  throw PyException("Python interpolate method failed");
	}
	else {
	  throw PyPyErrorException();
	}
      }
      bool res=PyListToConfig(result,out);
      if(!res) {
	Py_DECREF(result);
	throw PyException("Python interpolate method did not return a list");
      }
      Py_DECREF(result);
    }
  }

  virtual void Properties(PropertyMap& props) const
  {
    props = properties;
    if(!distance) {
      props.set("euclidean",1);
      props.set("metric","euclidean");
      if(!interpolate)
	props.set("geodesic",1);
    }
  }

  bool AddFeasibleDependency(const char* name,const char* dependency)
  {
    if(constraintMap.count(name)==0 || constraintMap.count(dependency)==0) return false;
    if(feasibleTestDeps.empty())
      feasibleTestDeps.resize(feasibleTests.size());
    int cindex = constraintMap[name];
    int dindex = constraintMap[dependency]; 
    feasibleTestDeps[cindex].push_back(dindex);
    return true;
  }

  bool AddVisibleDependency(const char* name,const char* dependency)
  {
    if(constraintMap.count(name)==0 || constraintMap.count(dependency)==0) return false;
    if(visibleTestDeps.empty())
      visibleTestDeps.resize(visibleTests.size());
    int cindex = constraintMap[name];
    int dindex = constraintMap[dependency]; 
    visibleTestDeps[cindex].push_back(dindex);
    return true;
  }


  void OptimizeQueryOrder() {
    if(!adaptive) return;
    OptimizeTestingOrder(feasibleStats,feasibleTestDeps,feasibleTestOrder);
    OptimizeTestingOrder(visibleStats,visibleTestDeps,visibleTestOrder);
  }

  PyObject *sample,
    *sampleNeighborhood,
    *distance,
    *interpolate;
  vector<PyObject*> feasibleTests;
  vector<PyObject*> visibleTests;
  vector<string> constraintNames;
  map<string,int> constraintMap;
  double edgeResolution;
  PropertyMap properties;

  bool adaptive;
  vector<TesterStats> feasibleStats,visibleStats;
  vector<vector<int> > feasibleTestDeps,visibleTestDeps;
  vector<int> feasibleTestOrder,visibleTestOrder;
  Timer timer;

  const Config *cacheq,*cacheq2;
  PyObject *cachex,*cachex2;
};



class PyEdgePlanner : public EdgePlanner
{
public:
  PyCSpace* space;
  Config a;
  Config b;
  int obstacle;

  PyEdgePlanner(PyCSpace* _space,const Config& _a,const Config& _b,int _obstacle=-1)
    :space(_space),a(_a),b(_b),obstacle(_obstacle)
  {}
  virtual ~PyEdgePlanner() {}
  virtual bool IsVisible() {
    assert(space->visibleTests.size() == space->feasibleTests.size());
    PyObject* pya = space->UpdateTempConfig(a);
    PyObject* pyb = space->UpdateTempConfig2(b);
    if(obstacle < 0) { //test all obstacles
      PyObject* args = PyTuple_New(2);
      Py_INCREF(pya);
      Py_INCREF(pyb);
      PyTuple_SetItem(args, 0, pya);
      PyTuple_SetItem(args, 1, pya);
      for(size_t i=0;i<space->visibleTests.size();i++) {
        int obs = (space->visibleTestOrder.empty() ? (int)i : space->visibleTestOrder[i]);
        if(space->visibleTests[obs] == NULL) {
          stringstream ss;
          ss<<"Python visible test for constraint "<<space->constraintNames[obs]<<"not defined"<<endl;
          Py_DECREF(args);
          throw PyException(ss.str().c_str());
        }

        if(space->adaptive) space->timer.Reset();	
        PyObject* result = PyObject_CallObject(space->visibleTests[obs],args);
        if(!result) {
          Py_DECREF(pya);
          Py_DECREF(pyb);
          Py_DECREF(args);
          if(!PyErr_Occurred()) {
            throw PyException("Python visible method failed");
          }
          else {
            throw PyPyErrorException();
          }
        }
        if(!PyBool_Check(result) && !PyInt_Check(result)) {
          Py_DECREF(pya);
          Py_DECREF(pyb);
          Py_DECREF(args);
          Py_DECREF(result);
          throw PyException("Python visible test didn't return bool");
        }
        int res=PyObject_IsTrue(result);
        Py_DECREF(result);
        if(space->adaptive) 
          space->visibleStats[obs].update(space->timer.ElapsedTime(),res); 
        if(res != 1) {
          Py_DECREF(pya);
          Py_DECREF(pyb);
          Py_DECREF(args);
          return false;
        }
      }
      Py_DECREF(pya);
      Py_DECREF(pyb);
      Py_DECREF(args);
    }
    else {
      //call visibility test for one obstacle
      if(space->visibleTests[obstacle] == NULL) {
        stringstream ss;
        ss<<"Python visible test for constraint "<<space->constraintNames[obstacle]<<"not defined"<<endl;
        throw PyException(ss.str().c_str());
      }

      if(space->adaptive) space->timer.Reset(); 	
      PyObject* result = PyObject_CallFunctionObjArgs(space->visibleTests[obstacle],pya,pyb,NULL);
      if(!result) {
        if(!PyErr_Occurred()) {
          throw PyException("Python visible method failed");
        }
        else {
          throw PyPyErrorException();
        }
      }
      if(!PyBool_Check(result) && !PyInt_Check(result)) {
        Py_DECREF(result);
        throw PyException("Python visible test didn't return bool");
      }
      int res=PyObject_IsTrue(result);
      Py_DECREF(result);
      if(space->adaptive) 
        space->visibleStats[obstacle].update(space->timer.ElapsedTime(),res); 

      if(res != 1) {
        return false;
      }
    }
    return true;
  }
  virtual void Eval(double u,Config& x) const
  {
    return space->Interpolate(a,b,u,x);
  }
  virtual const Config& Start() const { return a; }
  virtual const Config& Goal() const { return b; }
  virtual CSpace* Space() const { return space; }
  virtual EdgePlanner* Copy() const { return new PyEdgePlanner(space,a,b,obstacle); }
  virtual EdgePlanner* ReverseCopy() const { return new PyEdgePlanner(space,b,a,obstacle); }
};


EdgePlanner* PyCSpace::LocalPlanner(const Config& a,const Config& b)
{
  if(visibleTests.empty()) {
    return new StraightLineEpsilonPlanner(this,a,b,edgeResolution); 
  }
  else {
    return new PyEdgePlanner(this,a,b);
  }
}

EdgePlanner* PyCSpace::LocalPlanner(const Config& a,const Config& b,int obstacle)
{
  if(visibleTests.empty()) {
    return MakeSingleObstacleBisectionPlanner(this,a,b,obstacle,edgeResolution); 
  }
  else {
    return new PyEdgePlanner(this,a,b,obstacle);
  }
}

class PyGoalSet : public PiggybackCSpace
{
public:
  PyObject* goalTest,*sampler;
  PyGoalSet(CSpace* baseSpace,PyObject* _goalTest,PyObject* _sampler=NULL)
    :PiggybackCSpace(baseSpace),goalTest(_goalTest),sampler(_sampler)
  {
    Py_INCREF(goalTest);
    if(sampler)
      Py_INCREF(sampler);
  }
  ~PyGoalSet() {
    Py_DECREF(goalTest);
    if(sampler)
      Py_DECREF(sampler);
  }
  virtual void Sample(Config& x) {
    if(sampler) {
      //sample using python
      PyObject* result = PyObject_CallFunctionObjArgs(sampler,NULL);
      if(result == NULL) {
	if(!PyErr_Occurred()) {
	  throw PyException("Error calling goal sampler provided to setEndpoints, must accept 0 arguments");
	}
	else {
	  throw PyPyErrorException();
	}
      }
      PyListToConfig(result,x);
      Py_DECREF(result);
    }
    else PiggybackCSpace::Sample(x);
  }
  virtual bool IsFeasible(const Config& q) {
    PyObject* pyq = ToPy(q);
    PyObject* result = PyObject_CallFunctionObjArgs(goalTest,pyq,NULL);
    Py_DECREF(pyq);
    if(result == NULL) {
      if(!PyErr_Occurred()) {
	throw PyException("Error calling goal sampler provided to setEndpoints, must accept 1 argument");
      }
      else {
	throw PyPyErrorException();
      }
    }
    if(!PyBool_Check(result) && !PyInt_Check(result)) {
      Py_DECREF(result);
      throw PyException("Python visible test didn't return bool");
    }
    int res=PyObject_IsTrue(result);
    Py_DECREF(result);
    return res == 1;
  }
};




static vector<SmartPointer<PyCSpace> > spaces;
static vector<SmartPointer<MotionPlannerInterface> > plans;
static vector<SmartPointer<PyGoalSet> > goalSets;
static MotionPlannerFactory factory;
static list<int> spacesDeleteList;
static list<int> plansDeleteList;

int makeNewCSpace()
{
  if(spacesDeleteList.empty()) {
    spaces.push_back(new PyCSpace);
    return (int)(spaces.size()-1);
  }
  else {
    int index = spacesDeleteList.front();
    spacesDeleteList.erase(spacesDeleteList.begin());
    spaces[index] = new PyCSpace;
    return index;
  }
}

void destroyCSpace(int cspace)
{
  if(cspace < 0 || cspace >= (int)spaces.size()) 
    throw PyException("Invalid cspace index");
  spaces[cspace] = NULL;
  spacesDeleteList.push_back(cspace);
}

CSpaceInterface::CSpaceInterface()
{
  index = makeNewCSpace();
}

CSpaceInterface::CSpaceInterface(const CSpaceInterface& space)
{
  index = makeNewCSpace();
  *spaces[index] = *spaces[space.index];
}

CSpaceInterface::~CSpaceInterface()
{
  this->destroy();
}

void CSpaceInterface::destroy()
{
  if(index >= 0) {
    destroyCSpace(index);
    index = -1;
  }
}

void CSpaceInterface::setFeasibility(PyObject* pyFeas)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  for(size_t i=0;i<spaces[index]->feasibleTests.size();i++)
    Py_XDECREF(spaces[index]->feasibleTests[i]);
  Py_XINCREF(pyFeas);
  spaces[index]->feasibleTests.resize(1);
  spaces[index]->feasibleStats.resize(1);
  spaces[index]->constraintNames.resize(1);
  spaces[index]->constraintNames[0] = "feasible";
  spaces[index]->constraintMap.clear();
  spaces[index]->constraintMap["feasible"]=0;
  spaces[index]->feasibleTests[0] = pyFeas;
  spaces[index]->feasibleStats[0].reset(0,0,0);
  spaces[index]->feasibleTestOrder.resize(0);
}


void CSpaceInterface::addFeasibilityTest(const char* name,PyObject* pyFeas)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  int cindex = -1;
  if(spaces[index]->constraintMap.count(name) > 0)
    cindex = spaces[index]->constraintMap[name];
  spaces[index]->feasibleTests.resize(spaces[index]->constraintNames.size(),NULL);
  spaces[index]->feasibleStats.resize(spaces[index]->constraintNames.size());
  if(cindex < 0) {
    cindex = spaces[index]->feasibleTests.size();
    Py_XINCREF(pyFeas);
    spaces[index]->feasibleTests.push_back(pyFeas);
    spaces[index]->constraintNames.push_back(name);
    spaces[index]->feasibleStats.push_back(TesterStats());
    spaces[index]->constraintMap[name] = cindex;
  }
  else {
    Py_DECREF(spaces[index]->feasibleTests[cindex]);
    Py_XINCREF(pyFeas);
    spaces[index]->feasibleTests[cindex] = pyFeas;
    spaces[index]->feasibleStats[cindex].reset(0,0,0);
  }
  if(!spaces[index]->feasibleTestOrder.empty())
    spaces[index]->feasibleTestOrder.back() = cindex;
}

void CSpaceInterface::setVisibility(PyObject* pyVisible)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  for(size_t i=0;i<spaces[index]->visibleTests.size();i++)
    Py_XDECREF(spaces[index]->visibleTests[i]);
  Py_XINCREF(pyVisible);
  spaces[index]->visibleTests.resize(1);
  spaces[index]->visibleStats.resize(1);
  spaces[index]->visibleTests[0] = pyVisible;
  spaces[index]->visibleStats[0].reset(0,0,0);
  spaces[index]->visibleTestOrder.resize(0);
}

void CSpaceInterface::addVisibilityTest(const char* name,PyObject* pyVis)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  int cindex = -1;
  if(spaces[index]->constraintMap.count(name) > 0)
    cindex = spaces[index]->constraintMap[name];
  spaces[index]->visibleTests.resize(spaces[index]->constraintNames.size(),NULL);
  spaces[index]->visibleStats.resize(spaces[index]->constraintNames.size());
  if(cindex < 0) {
    cindex = (int)spaces[index]->visibleTests.size();
    Py_XINCREF(pyVis);
    spaces[index]->visibleTests.push_back(pyVis);
    spaces[index]->visibleStats.push_back(TesterStats());
    spaces[index]->constraintNames.push_back(name);
    spaces[index]->constraintMap[name] = cindex;
  }
  else {
    Py_DECREF(spaces[index]->visibleTests[cindex]);
    Py_XINCREF(pyVis);
    spaces[index]->visibleTests[cindex] = pyVis;
    spaces[index]->visibleStats[cindex].reset(0,0,1);
  }
}

void CSpaceInterface::setVisibilityEpsilon(double eps)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  if(eps <= 0) 
    throw PyException("Invalid epsilon");
  for(size_t i=0;i<spaces[index]->visibleTests.size();i++)
    Py_XDECREF(spaces[index]->visibleTests[i]);
  spaces[index]->visibleTests.resize(0);
  spaces[index]->visibleStats.resize(0);
  spaces[index]->visibleTestOrder.resize(0);
  spaces[index]->edgeResolution = eps;
}

void CSpaceInterface::setSampler(PyObject* pySamp)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  Py_XDECREF(spaces[index]->sample);
  Py_XINCREF(pySamp);
  spaces[index]->sample = pySamp;
}

void CSpaceInterface::setNeighborhoodSampler(PyObject* pySamp)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  Py_XDECREF(spaces[index]->sampleNeighborhood);
  Py_XINCREF(pySamp);
  spaces[index]->sampleNeighborhood = pySamp;
}

void CSpaceInterface::setDistance(PyObject* pyDist)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  Py_XDECREF(spaces[index]->distance);
  Py_XINCREF(pyDist);
  spaces[index]->distance = pyDist;
}

void CSpaceInterface::setInterpolate(PyObject* pyInterp)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  Py_XDECREF(spaces[index]->interpolate);
  Py_XINCREF(pyInterp);
  spaces[index]->interpolate = pyInterp;
}

void CSpaceInterface::setProperty(const char* key,const char* value)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  spaces[index]->properties[key] = value;
}

const char* CSpaceInterface::getProperty(const char* key)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  if(spaces[index]->properties.count(key)==0) 
    throw PyException("Invalid property");
  return spaces[index]->properties[key].c_str();
}


///queries
bool CSpaceInterface::isFeasible(PyObject* q)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  Config vq;
  if(!PyListToConfig(q,vq)) {
    throw PyException("Invalid configuration (must be list)");
  }
  return spaces[index]->IsFeasible(vq);
}

bool CSpaceInterface::isVisible(PyObject* a,PyObject* b)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  Config va,vb;
  if(!PyListToConfig(a,va)) {
    throw PyException("Invalid configuration a (must be list)");
  }
  if(!PyListToConfig(b,vb)) {
    throw PyException("Invalid configuration b (must be list)");
  }
  return spaces[index]->IsVisible(va,vb);
}

bool CSpaceInterface::testFeasibility(const char* name,PyObject* q)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  Config vq;
  if(!PyListToConfig(q,vq)) {
    throw PyException("Invalid configuration (must be list)");
  }
  int cindex = -1;
  if(spaces[index]->constraintMap.count(name)==0)
     throw PyException("Invalid constraint name");
  cindex = spaces[index]->constraintMap[name];
  return spaces[index]->IsFeasible(vq,cindex);
}

bool CSpaceInterface::testVisibility(const char* name,PyObject* a,PyObject* b)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  Config va,vb;
  if(!PyListToConfig(a,va)) {
    throw PyException("Invalid configuration a (must be list)");
  }
  if(!PyListToConfig(b,vb)) {
    throw PyException("Invalid configuration b (must be list)");
  }
  int cindex = -1;
  if(spaces[index]->constraintMap.count(name)==0)
     throw PyException("Invalid constraint name");
  index = spaces[index]->constraintMap[name];
  return spaces[index]->IsVisible(va,vb,cindex);
}

PyObject* CSpaceInterface::feasibilityFailures(PyObject* q)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) {
    printf("CSpace index %d is out of range [%d,%d) or was previously destroyed\n",index,0,spaces.size());
    throw PyException("Invalid cspace index");
  }
  Config vq;
  if(!PyListToConfig(q,vq)) {
    throw PyException("Invalid configuration (must be list)");    
  }
  vector<string> infeasible;
  spaces[index]->GetInfeasibleNames(vq,infeasible);
  return ToPy(infeasible);
}

PyObject* CSpaceInterface::visibilityFailures(PyObject* a,PyObject* b)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  Config va,vb;
  if(!PyListToConfig(a,va)) {
    throw PyException("Invalid configuration a (must be list)");
  }
  if(!PyListToConfig(b,vb)) {
    throw PyException("Invalid configuration b (must be list)");
  }
  vector<string> notVisible;
  for(size_t i=0;i<spaces[index]->feasibleTests.size();i++)
    if(!spaces[index]->IsVisible(va,vb,i)) notVisible.push_back(spaces[index]->constraintNames[i]);
  return ToPy(notVisible);
}

PyObject* CSpaceInterface::sample()
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  Config q;
  spaces[index]->Sample(q);
  return ToPy(q);
}

double CSpaceInterface::distance(PyObject* a,PyObject* b)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  Config va,vb;
  if(!PyListToConfig(a,va)) {
    throw PyException("Invalid configuration a (must be list)");
  }
  if(!PyListToConfig(b,vb)) {
    throw PyException("Invalid configuration b (must be list)");
  }
  return spaces[index]->Distance(va,vb);
}

PyObject* CSpaceInterface::interpolate(PyObject* a,PyObject* b,double u)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  Config va,vb,vout;
  if(!PyListToConfig(a,va)) {
    throw PyException("Invalid configuration a (must be list)");
  }
  if(!PyListToConfig(b,vb)) {
    throw PyException("Invalid configuration b (must be list)");
  }
  spaces[index]->Interpolate(va,vb,u,vout);
  return PyListFromConfig(vout);
}

bool CSpaceInterface::adaptiveQueriesEnabled()
{
  return false;
}
void CSpaceInterface::enableAdaptiveQueries(bool enabled)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  spaces[index]->adaptive = enabled;
}

void CSpaceInterface::optimizeQueryOrder()
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  spaces[index]->OptimizeQueryOrder();
}

void CSpaceInterface::setFeasibilityDependency(const char* name,const char* precedingTest)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  if(!spaces[index]->AddFeasibleDependency(name,precedingTest))
    throw PyException("Invalid dependency");
}

void CSpaceInterface::setFeasibilityPrior(const char* name,double costPrior,double feasibilityProbability,double evidenceStrength)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  int cindex = -1;
  if(spaces[index]->constraintMap.count(name)==0)
     throw PyException("Invalid constraint name");
  cindex = spaces[index]->constraintMap[name];
  spaces[index]->feasibleStats[cindex].reset(costPrior,feasibilityProbability,evidenceStrength);
}

void CSpaceInterface::setVisibilityDependency(const char* name,const char* precedingTest)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  if(!spaces[index]->AddVisibleDependency(name,precedingTest))
    throw PyException("Invalid dependency");
}

void CSpaceInterface::setVisibilityPrior(const char* name,double costPrior,double visibilityProbability,double evidenceStrength)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  int cindex = -1;
  if(spaces[index]->constraintMap.count(name)==0)
     throw PyException("Invalid constraint name");
  cindex = spaces[index]->constraintMap[name];
  spaces[index]->visibleStats[cindex].reset(costPrior,visibilityProbability,evidenceStrength);
}

double CSpaceInterface::feasibilityCost(const char* name)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  int cindex = -1;
  if(spaces[index]->constraintMap.count(name)==0)
     throw PyException("Invalid constraint name");
  cindex = spaces[index]->constraintMap[name];
  return spaces[index]->feasibleStats[cindex].cost;
}

double CSpaceInterface::feasibilityProbability(const char* name)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  int cindex = -1;
  if(spaces[index]->constraintMap.count(name)==0)
     throw PyException("Invalid constraint name");
  cindex = spaces[index]->constraintMap[name];
  return spaces[index]->feasibleStats[cindex].probability;
}

double CSpaceInterface::visibilityCost(const char* name)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  int cindex = -1;
  if(spaces[index]->constraintMap.count(name)==0)
     throw PyException("Invalid constraint name");
  cindex = spaces[index]->constraintMap[name];
  return spaces[index]->visibleStats[cindex].cost;
}

double CSpaceInterface::visibilityProbability(const char* name)
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  int cindex = -1;
  if(spaces[index]->constraintMap.count(name)==0)
     throw PyException("Invalid constraint name");
  cindex = spaces[index]->constraintMap[name];
  return spaces[index]->visibleStats[cindex].probability;
}

PyObject* CSpaceInterface::feasibilityQueryOrder()
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  PyObject* res = PyList_New(spaces[index]->feasibleTests.size());
  for(size_t i=0;i<spaces[index]->constraintNames.size();i++) {
    int cindex = (spaces[index]->feasibleTestOrder.empty() ? (int)i : spaces[index]->feasibleTestOrder[i]);
    PyObject* s = PyString_FromString(spaces[index]->constraintNames[cindex].c_str());
    PyList_SetItem(res,i,s);
  }
  return res;
}

PyObject* CSpaceInterface::visibilityQueryOrder()
{
  if(index < 0 || index >= (int)spaces.size() || spaces[index]==NULL) 
    throw PyException("Invalid cspace index");
  PyObject* res = PyList_New(spaces[index]->visibleTests.size());
  for(size_t i=0;i<spaces[index]->constraintNames.size();i++) {
    int cindex = (spaces[index]->visibleTestOrder.empty() ? (int)i : spaces[index]->visibleTestOrder[i]);
    PyObject* s = PyString_FromString(spaces[index]->constraintNames[cindex].c_str());
    PyList_SetItem(res,i,s);
  }
  return res;
}



void setPlanJSONString(const char* string)
{
  if(!factory.LoadJSON(string))
    throw PyException("Invalid JSON string");
}

std::string getPlanJSONString()
{
  return factory.SaveJSON();
}

void setPlanType(const char* type)
{
  factory.type = type;
}

void setPlanSetting(const char* setting,double value)
{
  //printf("Setting factory setting %s to %g\n",setting,value);
  if(0==strcmp(setting,"knn")) 
    factory.knn = (int)value;
  else if(0==strcmp(setting,"connectionThreshold"))
    factory.connectionThreshold = value;
  else if(0==strcmp(setting,"perturbationRadius"))
    factory.perturbationRadius = value;
  else if(0==strcmp(setting,"bidirectional"))
    factory.bidirectional = (bool)(int)(value);
  else if(0==strcmp(setting,"grid"))
    factory.useGrid = (bool)(int)(value);
  else if(0==strcmp(setting,"gridResolution"))
    factory.gridResolution = value;
  else if(0==strcmp(setting,"suboptimalityFactor")) 
    factory.suboptimalityFactor = value;
  else if(0==strcmp(setting,"ignoreConnectedComponents")) 
    factory.ignoreConnectedComponents = (bool)(int)(value);
  else if(0==strcmp(setting,"randomizeFrequency"))
    factory.randomizeFrequency = (int)value;
  else if(0==strcmp(setting,"shortcut"))
    factory.shortcut = (value != 0);
  else if(0==strcmp(setting,"restart"))
    factory.restart = (value != 0);
  else {
    throw PyException("Invalid setting");
  }
}

void setPlanSetting(const char* setting,const char* value)
{
  if(0==strcmp(setting,"pointLocation"))
    factory.pointLocation = value;
  else if(0==strcmp(setting,"restartTermCond"))
    factory.restartTermCond = value;
  else {
    throw PyException("Invalid setting");
  }
}


int makeNewPlan(int cspace)
{
  if(cspace < 0 || cspace >= (int)spaces.size() || spaces[cspace]==NULL) 
    throw PyException("Invalid cspace index");
  if(plansDeleteList.empty()) {
    plans.push_back(factory.Create(spaces[cspace]));
    return (int)plans.size()-1;
  }
  else {
    int index = plansDeleteList.front();
    plansDeleteList.erase(plansDeleteList.begin());
    plans[index] = factory.Create(spaces[cspace]);
    return index;
  }
}

void destroyPlan(int plan)
{
  if(plan < 0 || plan >= (int)plans.size() || plans[plan]==NULL) 
    throw PyException("Invalid plan index");
  plans[plan] = NULL;
  if(plan < (int)goalSets.size())
    goalSets[plan] = NULL;
  plansDeleteList.push_back(plan);
}

PlannerInterface::PlannerInterface(const CSpaceInterface& cspace)
{
  index = makeNewPlan(cspace.index);
  spaceIndex = cspace.index;
}

PlannerInterface::~PlannerInterface()
{
  this->destroy();
}

void PlannerInterface::destroy()
{
  if(index >= 0) {
    destroyPlan(index);
    index = -1;
  }
}

bool PlannerInterface::setEndpoints(PyObject* start,PyObject* goal)
{
  if(index < 0 || index >= (int)plans.size() || plans[index]==NULL) 
    throw PyException("Invalid plan index");
  Config qstart,qgoal;
  bool res=PyListToConfig(start,qstart);
  if(!res) 
    throw PyException("Invalid start endpoint");
  if(!spaces[spaceIndex]->IsFeasible(qstart)) {
    throw PyException("Start configuration is infeasible");
  }
  int istart=plans[index]->AddMilestone(qstart);
  if(istart < 0) {
    throw PyException("Start configuration is infeasible");
  }
  if(istart != 0) {
    throw PyException("Plan already initialized?");
  }

  res=PyListToConfig(goal,qgoal);
  if(!res) 
    throw PyException("Invalid start endpoint");
  if(!spaces[spaceIndex]->IsFeasible(qgoal)) {
    throw PyException("Goal configuration is infeasible");
  }
  int igoal=plans[index]->AddMilestone(qgoal);
  if(igoal < 0) {
    throw PyException("Goal configuration is infeasible");
  }
  return true;
}
bool PlannerInterface::setEndpointSet(PyObject* start,PyObject* goal,PyObject* goalSample)
{
  if(index < 0 || index >= (int)plans.size() || plans[index]==NULL) 
    throw PyException("Invalid plan index");
  Config qstart;
  bool res=PyListToConfig(start,qstart);
  if(!res) 
    throw PyException("Invalid start endpoint");
  if(!spaces[spaceIndex]->IsFeasible(qstart)) {
    throw PyException("Start configuration is infeasible");
  }
  //test if it's a goal test
  if(!PyCallable_Check(goal)) {
    throw PyException("Goal test is not callable");
  }
  goalSets.resize(plans.size());
  goalSets[index] = new PyGoalSet(spaces[spaceIndex],goal,goalSample);
  plans[index]=factory.Create(spaces[spaceIndex],qstart,goalSets[index]);
  return true;
}

int PlannerInterface::addMilestone(PyObject* milestone)
{
  if(index < 0 || index >= (int)plans.size() || plans[index]==NULL) 
    throw PyException("Invalid plan index");
  Config q;
  bool res=PyListToConfig(milestone,q);
  if(!res) 
    throw PyException("Invalid milestone provided to addMilestone");
  int mindex=plans[index]->AddMilestone(q);
  return mindex;
}

void DumpPlan(MotionPlannerInterface* planner,const char* fn)
{
  RoadmapPlanner prm(NULL);
  planner->GetRoadmap(prm);
  
  Graph::Graph<string,string> Gstr;
  Graph::NodesToStrings(prm.roadmap,Gstr);
  
  ofstream out(fn);
  Graph::Save_TGF(out,Gstr);
  out.close();
}

void PlannerInterface::planMore(int iterations)
{
  if(index < 0 || index >= (int)plans.size() || plans[index]==NULL) 
    throw PyException("Invalid plan index");
  spaces[spaceIndex]->OptimizeQueryOrder();
  plans[index]->PlanMore(iterations);
  //printf("Plan now has %d milestones, %d components\n",plans[plan]->NumMilestones(),plans[plan]->NumComponents());
  //DumpPlan(plans[plan],"plan.tgf");
}

PyObject* PlannerInterface::getPathEndpoints()
{
  if(index < 0 || index >= (int)plans.size() || plans[index]==NULL) 
    throw PyException("Invalid plan index");  
  if(!plans[index]->IsSolved()) {
    Py_RETURN_NONE;
  }
  MilestonePath path;
  plans[index]->GetSolution(path);
  PyObject* pypath = PyList_New(path.NumMilestones());
  for(int i=0;i<path.NumMilestones();i++)
    PyList_SetItem(pypath,(Py_ssize_t)i,PyListFromConfig(path.GetMilestone(i)));
  return pypath;
}

PyObject* PlannerInterface::getPath(int milestone1,int milestone2)
{
  if(index < 0 || index >= (int)plans.size() || plans[index]==NULL) 
    throw PyException("Invalid plan index");  
  if(!plans[index]->IsConnected(milestone1,milestone2)) {
    Py_RETURN_NONE;
  }
  MilestonePath path;
  plans[index]->GetPath(milestone1,milestone2,path);
  PyObject* pypath = PyList_New(path.NumMilestones());
  for(int i=0;i<path.NumMilestones();i++)
    PyList_SetItem(pypath,(Py_ssize_t)i,PyListFromConfig(path.GetMilestone(i)));
  return pypath;
}


double PlannerInterface::getData(const char* setting)
{
  if(index < 0 || index >= (int)plans.size() || plans[index]==NULL) 
    throw PyException("Invalid plan index");  
  if(0==strcmp(setting,"iterations")) {
    return plans[index]->NumIterations();
  }
  else if(0==strcmp(setting,"milestones")) {
    return plans[index]->NumMilestones();
  }
  else if(0==strcmp(setting,"components")) {
    return plans[index]->NumComponents();
  }
  else {
    throw PyException("Invalid plan option");
    return 0;
  }
}

PyObject* PlannerInterface::getStats()
{
  if(index < 0 || index >= (int)plans.size() || plans[index]==NULL) 
    throw PyException("Invalid plan index");  
  PropertyMap stats;
  plans[index]->GetStats(stats);
  PyObject* res = PyDict_New();
  for(PropertyMap::const_iterator i=stats.begin();i!=stats.end();i++) {
    PyObject* value = PyString_FromString(i->second.c_str());
    PyDict_SetItemString(res,i->first.c_str(),value);
    Py_XDECREF(value);
  }
  return res;
}

PyObject* PlannerInterface::getRoadmap()
{
  if(index < 0 || index >= (int)plans.size() || plans[index]==NULL) 
    throw PyException("Invalid plan index");
  RoadmapPlanner prm(NULL);
  plans[index]->GetRoadmap(prm);
  PyObject* pyV = PyList_New(prm.roadmap.nodes.size());
  for(size_t i=0;i<prm.roadmap.nodes.size();i++)
    PyList_SetItem(pyV,(Py_ssize_t)i,PyListFromConfig(prm.roadmap.nodes[i]));
  PyObject* pyE = PyList_New(0);
  for(size_t i=0;i<prm.roadmap.nodes.size();i++) {
    RoadmapPlanner::Roadmap::Iterator e;
    for(prm.roadmap.Begin(i,e);!e.end();e++) {
      PyObject* pair = Py_BuildValue("(ii)",e.source(),e.target());
      PyList_Append(pyE,pair);
      Py_XDECREF(pair);
    }
  }
  //this steals the references
  return Py_BuildValue("NN",pyV,pyE);
}

void PlannerInterface::dump(const char* fn)
{
  if(index < 0 || index >= (int)plans.size() || plans[index]==NULL) 
    throw PyException("Invalid plan index");
  DumpPlan(plans[index],fn);
}

void destroy()
{
  spaces.resize(0);
  spacesDeleteList.resize(0);
  plans.resize(0);
  plansDeleteList.resize(0);
}
