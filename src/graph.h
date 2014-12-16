#ifndef SIMIT_GRAPH_H
#define SIMIT_GRAPH_H

#include <cstddef>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <ostream>

#include "tensor_components.h"
#include "variadic.h"
#include "error.h"

namespace simit {

// Forward declarations
class SetBase;
template <typename T, int... dimensions> class FieldRef;
template <typename T, int... dimensions> class TensorRef;

namespace {
class FieldRefBase;
}

namespace internal {
class VertexToEdgeEndpointIndex;
class VertexToEdgeIndex;
class NeighborIndex;
}

class Function;

/// A Simit element reference.  All Simit elements live in Simit sets and an
/// ElementRef provides a reference to an element.
class ElementRef {
public:
  inline ElementRef() : ident(-1) {}

  bool defined() const {return ident != -1;}

  friend inline bool operator==(const ElementRef& e1, const ElementRef& e2) {
    return e1.ident == e2.ident;
  }

  friend inline bool operator!=(const ElementRef& e1, const ElementRef& e2) {
    return e1.ident != e2.ident;
  }

  friend inline bool operator<(const ElementRef& e1, const ElementRef& e2) {
    return e1.ident < e2.ident;
  }

  friend inline bool operator>(const ElementRef& e1, const ElementRef& e2) {
    return e1.ident > e2.ident;
  }

  friend inline bool operator<=(const ElementRef& e1, const ElementRef& e2) {
    return e1.ident <= e2.ident;
  }

  friend inline bool operator>=(const ElementRef& e1, const ElementRef& e2) {
    return e1.ident >= e2.ident;
  }

  friend std::ostream &operator<<(std::ostream &os, const ElementRef& er) {
    return os << er.ident;
  }

  int ident;

private:
  explicit inline ElementRef(int ident) : ident(ident) {}

  friend class SetBase;
  friend class FieldRefBase;
  friend class internal::VertexToEdgeEndpointIndex;
  friend class internal::VertexToEdgeIndex;
  friend class internal::NeighborIndex;
};


// Base class for Sets
// Sets are used to represent collections within C++,
// and can be passed as bound inputs to Simit programs.
class SetBase {
public:
  SetBase() :  numElements(0), cardinality(0),
               endpoints(nullptr), capacity(capacityIncrement),
               neighbors(nullptr) {}

  template <typename ...T>
  SetBase(const T& ...sets) : SetBase() {
    this->cardinality  = sizeof...(sets);
    this->endpointSets = epsMaker(endpointSets, sets...);
    this->endpoints    = (int*) calloc(sizeof(int), capacity*cardinality);
  }

  ~SetBase() {
    for (auto f: fields) {
      delete f;
    }
    free(endpoints);
  }
  
  /// Return the number of elements in the Set
  int getSize() const { return numElements; }

  /// Return the number of endpoints of the elements in the set.  Non-edge sets
  /// have cardinality 0.
  int getCardinality() const { return cardinality; }

  /// Add a tensor field to the set.  Use the template parameters to specify the
  /// component type and dimension sizes of the tensors.  For example, define a
  /// field of 2x3 matrices containing doubles as follows:
  /// Field<double,2,3> matrix = addField<double,2,3>("mat");
  template <typename T, int... dimensions>
  FieldRef<T, dimensions...> addField(const std::string &name) {
    FieldData::TensorType *type =
        new FieldData::TensorType(typeOf<T>(), {dimensions...});
    FieldData *fieldData = new FieldData(name, type, this);
    fieldData->data = calloc(capacity, fieldData->sizeOfType);
    fields.push_back(fieldData);
    fieldNames[name] = fields.size()-1;
    return FieldRef<T, dimensions...>(fieldData);
  }
  
  /// Get a Field corresponding to the string fieldName
  template <typename T, int... dimensions>
  FieldRef<T, dimensions...> getField(std::string fieldName) {
    // need to check if the field actually exists because maps just add an entry
    // if none exists
    uassert(fieldNames.find(fieldName) != fieldNames.end())
        << "Invalid field name in getField()";
    FieldData *fieldData = fields[fieldNames[fieldName]];
    uassert(typeOf<T>() == fieldData->type->getComponentType())
        << "Incorrect field type.";
    return FieldRef<T, dimensions...>(fieldData);
  }

  /// Add a new element or edge, returning its handle.
  /// The endpoints refer to the respective Sets they come from.
  template <typename ...Endpoints>
  ElementRef add(Endpoints... endpoints) {
    iassert(sizeof...(endpoints) == cardinality) <<"Wrong number of endpoints.";
    if (numElements > capacity-1)
      increaseEdgeCapacity();
    
    addEndpoints(0, endpoints...);

    if (numElements > capacity-1)
      increaseCapacity();
    return ElementRef(numElements++);
  }

  /// Remove an element from the Set
  void remove(ElementRef element) {
    for (auto f : fields){
      iassert(isValidComponentType(f->type->getComponentType()));
      switch (f->type->getComponentType()) {
        case ComponentType::FLOAT: {
          double* data = (double*)f->data;
          data[element.ident] = data[numElements-1];
          break;
        }
        case ComponentType::INT: {
          int* data = (int*)f->data;
          data[element.ident] = data[numElements-1];
          break;
        }
      }
    }
    numElements--;
  }

  /// Iterator that iterates over the elements in a Set
  ///
  /// This iterator is an input_iterator, and thus can only be
  /// dereferenced as an rvalue.  Furthermore, it can only return
  /// const references to Elements.
  class ElementIterator {
  public:
    // some typedefs to make interop with std easier
    typedef std::input_iterator_tag iterator_category;
    typedef ElementRef value_type;
    typedef ptrdiff_t difference_type;
    typedef ElementRef& reference;
    typedef ElementRef* pointer;

    ElementIterator(const SetBase* set, int idx=0) : curElem(idx), set(set) { }
    ElementIterator(const ElementIterator& other) : curElem(other.curElem),
    set(other.set) {}

    reference operator*() {return curElem;}
    pointer operator->() {return &curElem;}

    ElementIterator& operator++() {
      curElem.ident++;
      return *this;
    }

    ElementIterator operator++(int) {
      curElem.ident++;
      return *this;
    }

    friend bool operator!=(const ElementIterator& l, const ElementIterator& r) {
      return !(l.set==r.set) || !(l.curElem == r.curElem);
    }

    friend bool operator==(const ElementIterator& l, const ElementIterator& r) {
      return (l.set==r.set) && (l.curElem == r.curElem);
    }

    friend bool operator<(const ElementIterator& l, const ElementIterator& r) {
      iassert(l.set == r.set);
      return l.curElem < r.curElem;
    }

  private:
    ElementRef curElem; // current element index
    const SetBase* set; // set we're iterating over
  };

  /// Create an ElementIterator for this Set, set to the first element
  ElementIterator begin() const { return ElementIterator(this, 0); }
  
  /// Create an ElementIterator for terminating iteration over this Set
  ElementIterator end() const { return ElementIterator(this, getSize()); }

  /// Get the endpoint set at the given location.
  const SetBase *getEndpointSet(int loc) const {
    return endpointSets[loc];
  }

  /// A set is homogeneous of all it's endpoints come from the same set,
  /// otherwise it is heterogeneous.
  bool isHomogeneous() const {
    if (cardinality > 0) {
      const SetBase *firstEndpointSet = getEndpointSet(0);
      for (int i=1; i < cardinality; ++i) {
        // Endpointsets are the same if their pointers point at the same set
        if (getEndpointSet(i) != firstEndpointSet) {
          return false;
        }
      }
    }
    return true;
  }

  /// Get an endpoint of an edge
  ElementRef getEndpoint(ElementRef edge, int endpointNum) const {
    return ElementRef(endpoints[edge.ident*cardinality+endpointNum]);
  }
  
  /// Iterator that iterates over the endpoints of an edge
  class EndpointIterator {
  public:
    // some typedefs to make interop with std easier
    typedef std::input_iterator_tag iterator_category;
    typedef ElementRef value_type;
    typedef ptrdiff_t difference_type;
    typedef ElementRef& reference;
    typedef ElementRef* pointer;

    EndpointIterator(const SetBase *set, ElementRef elem, int endpointN=0) :
    curElem(elem), retElem(-1), endpointNum(endpointN), set(set) {
      retElem = set->getEndpoint(curElem, endpointNum);
    }
    EndpointIterator(const EndpointIterator& other) : curElem(other.curElem),
    retElem(other.retElem), endpointNum(other.endpointNum), set(other.set) { }

    reference operator*() {
      return retElem;
    }

    pointer operator->()  {
      return &retElem;
    }

    EndpointIterator& operator++() {
      const int cardinality = set->getCardinality();
      endpointNum++;
      if (endpointNum > set->cardinality-1)
        retElem.ident = -1;   // return invalid element
      else
        retElem.ident = set->endpoints[curElem.ident*cardinality+endpointNum];
      return *this;
    }

    EndpointIterator operator++(int) {
      const int cardinality = set->getCardinality();
      endpointNum++;
      if (endpointNum > cardinality-1)
        retElem.ident = -1;   // return invalid element
      else
        retElem.ident = set->endpoints[curElem.ident*cardinality+endpointNum];
      return *this;
    }

    bool operator!=(const EndpointIterator& other) {
      return !(set==other.set) || !(curElem.ident == other.curElem.ident) ||
      !(endpointNum==other.endpointNum);
    }

    bool operator==(const EndpointIterator& other) {
      return (set==other.set) && (curElem.ident == other.curElem.ident) &&
      (endpointNum==other.endpointNum);
    }

    bool lessThan(const EndpointIterator &other) const {
      iassert(this->set == other.set)
          << "Comparing EndpointIterators from two different Sets";
      iassert(this->curElem.ident == other.curElem.ident)
          << "Comparing EndpointIterators over two different edges";
      return this->endpointNum < other.endpointNum;
    }

    friend inline bool operator<(const EndpointIterator &e1,
                                 const EndpointIterator &e2) {
      return e1.lessThan(e2);
    }

  private:
    ElementRef curElem;     // current element index
    ElementRef retElem;     // element we're returning
    int endpointNum;        // the current endpoint number
    const SetBase *set;     // set we're iterating over
  };
  
  /// Start iterator for endpoints of an edge
  EndpointIterator endpoints_begin(ElementRef edge) {
    return EndpointIterator(this, edge, 0);
  }
  
  /// End iterator for endpoints of an edge
  EndpointIterator endpoints_end(ElementRef edge) {
    return EndpointIterator(this, edge, cardinality);
  }

  const void *getFieldData(const std::string &fieldName) const {
    iassert(fieldNames.find(fieldName) != fieldNames.end());
    return fields[fieldNames.at(fieldName)]->data;
  }

  /// Get an array containing, for each edge in a set, the elements it connects.
  const int *getEndpointsData() const {return endpoints;}

  /// If this set is an edge set with cardinality 2 then return an index that
  /// for each element in the first connected set contains it's neighbors in the
  /// second connceted set. Otherwise, return nullptr.
  const internal::NeighborIndex *getNeighborIndex() const;

  friend std::ostream &operator<<(std::ostream &os, const SetBase &set) {
    return set.streamOut(os);
  }

private:
  int numElements;                           // number of elements in the set
  int cardinality;                           // number of element endpoints
  std::vector<const SetBase*> endpointSets;  // the sets the endpoints belong to
  int* endpoints;                            // the endpoints of edge elements

  int capacity;                              // current capacity of the set
  static const int capacityIncrement = 1024; // increment for capacity increases

  mutable internal::NeighborIndex *neighbors;// neighbor index (lazily created)

  std::ostream &streamOut(std::ostream &os) const {
    os << "{";
    auto it = begin();
    auto it_end = end();
    if (it != it_end) {
      os << it->ident;
      if (getCardinality() > 0) {
        os << ":(";
        os << endpoints[0];
        for (int i=1; i<getCardinality(); ++i) {
          os << "," << endpoints[i];
        }
        os << ")";
      }
      ++it;
    }
    while (it != it_end) {
      os << ", " << it->ident;
      if (getCardinality() > 0) {
        os << ":(";
        os << endpoints[it->ident + 0];
        for (int i=1; i<getCardinality(); ++i) {
          os << "," << endpoints[it->ident + i];
        }
        os << ")";
      }
      ++it;
    }
    return os << "}";
  }

  // A field on the members of the Set.
  // Invariant: elements < capacity
  struct FieldData {
    class TensorType {
    public:
      TensorType(ComponentType componentType, std::initializer_list<int> dims)
          : componentType(componentType), dimensions(dims), size(1) {
        for (auto dim : dimensions) {
          size *= dim;
        }
      }
      ComponentType getComponentType() const { return componentType; }
      size_t getOrder() const { return dimensions.size(); }
      size_t getDimension(size_t i) const {
        iassert(i<getOrder());
        return dimensions[i];
      }
      size_t getSize() const { return size; }
    private:
      ComponentType componentType;
      std::vector<int> dimensions;
      size_t size;
    };

    std::string name;
    const TensorType *type;
    size_t sizeOfType;
    SetBase *set;  // Set this field is a member of. Used for printing, etc.
    
    FieldData(const std::string &name, const TensorType *type, SetBase *set)
        : name(name), type(type), set(set), data(nullptr) {
      sizeOfType = componentSize(type->getComponentType()) * type->getSize();
    }

    ~FieldData() {
      if (data != nullptr) free(data);
      delete type;
    }
    
    void* data;         // buffer for the data

    /// Field references so that we can update their data pointers if we realloc
    /// field data.
    //  Note: his is a bit too complex, but avoids two loads on field get/set.
    std::set<FieldRefBase*> fieldReferences;
    
  private:
    /// disable copy constructors
    FieldData(const FieldData& f);
    FieldData& operator=(const FieldData& f);
  };

  std::vector<FieldData*> fields;          // fields of the elements in the set
  std::map<std::string, int> fieldNames; // name to field lookups
  
  /// disable copy constructors
  SetBase(const SetBase& s);
  SetBase& operator=(const SetBase& s);

  /// increase capacity of all fields
  void increaseCapacity();

  /// helpers for constructing endpoint sets
  template <typename F, typename ...T> std::vector<const SetBase*>
  epsMaker(std::vector<const SetBase*> sofar, const F& f, const T& ... sets) {
    sofar.push_back(&f);
    return epsMaker(sofar, sets...);
  }
  template <typename F> std::vector<const SetBase*>
  epsMaker(std::vector<const SetBase*> sofar, const F& f) {
    sofar.push_back(&f);
    return sofar;
  }
  std::vector<const SetBase*>
  epsMaker(std::vector<const SetBase*> sofar) {return sofar;}

  void increaseEdgeCapacity() {
    size_t newSize = (capacity+capacityIncrement)*cardinality*sizeof(int);
    endpoints = (int*)realloc(endpoints, newSize);
  }

  // helper for adding edges
  template <typename F, typename ...T>
  void addEndpoints(int which, F f, T ... eps) {
    uassert(endpointSets[which]->getSize() > f.ident)
        << "Invalid member of set in addEdge";
    endpoints[numElements*cardinality+which] = f.ident;
    addEndpoints(which+1, eps...);
  }
  template <typename F>
  void addEndpoints(int which, F f) {
    uassert(endpointSets[which]->getSize() > f.ident)
        << "Invalid member of set in addEdge";
    endpoints[numElements*cardinality+which] = f.ident;
  }
  void addEndpoints(int) {}

  friend FieldRefBase;
  friend Function;
};


template <int c=0>
class Set : public SetBase {
public:
  template <typename ...T>
  Set(const T& ... sets) : SetBase(sets...) {
    static_assert(sizeof...(sets) == c, "Wrong number of endpoint sets");
  }
};


// Field References
namespace {

/// The base class of field references.
class FieldRefBase {
public:
   /// Rule of five methods for copying and moving the field reference.  Note
   /// that there is quite a bit of machinery to maintain a fieldReferences
   /// set of live field references in Set::FieldData.  This is because the
   /// field data may be reallocated in which case the field reference data
   /// pointers must be updated.
  ~FieldRefBase() {
    this->fieldData->fieldReferences.erase(this);
  }

  FieldRefBase(const FieldRefBase& other) {
    data = other.data;
    fieldData = other.fieldData;
    this->fieldData->fieldReferences.insert(this);
  }

  FieldRefBase(FieldRefBase&& other) {
    std::swap (data, other.data);
    std::swap (fieldData, other.fieldData);
    this->fieldData->fieldReferences.erase(&other);
    this->fieldData->fieldReferences.insert(this);
  }

  FieldRefBase& operator=(const FieldRefBase &other) {
    data = other.data;
    fieldData = other.fieldData;
    this->fieldData->fieldReferences.insert(this);
    return *this;
  }

  FieldRefBase& operator=(FieldRefBase&& other) {
    std::swap(data, other.data);
    std::swap (fieldData, other.fieldData);
    this->fieldData->fieldReferences.erase(&other);
    this->fieldData->fieldReferences.insert(this);
    return *this;
  }

  // Return the field's data.  The data is a contigues sequence containing the
  // tensor of each element in no particular order.  The tensors are currently
  // laid out in row-major order, but this may change in the future.
  inline void *getData() {
    return static_cast<void*>(data);
  }

protected:
  FieldRefBase(void *fieldData)
      : fieldData(static_cast<SetBase::FieldData*>(fieldData)),
        data(this->fieldData->data) {
    this->fieldData->fieldReferences.insert(this);
  }

  template <typename T>
  inline T *getElemDataPtr(ElementRef element, size_t elementFieldSize) const {
    return &static_cast<T*>(data)[element.ident * elementFieldSize];
  }

  SetBase::FieldData *fieldData;

private:
  void *data;

  friend SetBase;
};

template <typename T, int... dimensions>
class FieldRefBaseParameterized : public FieldRefBase {
 public:
  TensorRef<T, dimensions...> get(ElementRef element) {
    return TensorRef<T, dimensions...>(getElemDataPtr(element));
  }

  const TensorRef<T, dimensions...> get(ElementRef element) const {
    return TensorRef<T, dimensions...>(getElemDataPtr(element));
  }

  void set(ElementRef element, std::initializer_list<T> values) {
    size_t tensorSize = TensorRef<T,dimensions...>::getSize();
    iassert(values.size() == tensorSize) << "Incorrect number of init values";
    T *elemData = this->getElemDataPtr(element);
    size_t i=0;
    for (T val : values) {
      elemData[i++] = val;
    }
  }

 protected:
  inline T *getElemDataPtr(ElementRef element) const {
    size_t elementFieldSize = TensorRef<T,dimensions...>::getSize();
    return FieldRefBase::getElemDataPtr<T>(element, elementFieldSize);
  }

  FieldRefBaseParameterized(void *fieldData) : FieldRefBase(fieldData) {}
};


} // unnamed namespace

template <typename T, int... dimensions>
class FieldRef : public FieldRefBaseParameterized<T,dimensions...> {
 private:
  FieldRef(void *fieldData)
      : FieldRefBaseParameterized<T,dimensions...>(fieldData) {}
  friend class SetBase;

  friend std::ostream &operator<<(std::ostream &os,
                                  const FieldRef<T, dimensions...> &field) {
    os << "[";
    auto it = field.fieldData->set->begin();
    auto end = field.fieldData->set->end();
    if (it != end) {
      os << field.get(*it);
      ++it;
    }

    while (it != end) {
      os << ", " << field.get(*it);
      ++it;
    }

    return os << "]";
  }
};

/// @cond SPECIALIZATION
template <typename T>
class FieldRef<T> : public FieldRefBaseParameterized<T> {
 public:
  void set(ElementRef element, T val) {
    (*this->getElemDataPtr(element)) = val;
  }

 private:
  FieldRef(void *fieldData) : FieldRefBaseParameterized<T>(fieldData) {}
  friend class SetBase;
};
/// @endcond

// Tensor References

template <typename T, int... dimensions>
class TensorRef {
 public:
  static size_t getOrder() {
    return sizeof...(dimensions);
  }

  static size_t getSize() { return util::product<dimensions...>::value; }

  inline TensorRef<T> &operator=(T val) {
    static_assert(sizeof...(dimensions) == 0,
                  "Can only assign scalar values to scalar tensors.");
    data[0] = val;
    return *this;
  }

  inline operator T() const {
    static_assert(sizeof...(dimensions) == 0,
                  "Can only convert scalar tensors to scalar values.");
    return data[0];
  }

  template <typename... Indices>
  inline T &operator()(Indices... index) {
    static_assert(sizeof...(dimensions) > 0,
                  "Access scalars directly, not through operator()");
    static_assert(sizeof...(index) == sizeof...(dimensions),
                  "Incorrect number of indices used to index tensor");
    auto dims = simit::util::seq<dimensions...>();
    return data[simit::util::computeOffset(dims, index...)];
  }

  template <typename... Indices>
  inline const T &operator()(Indices... index) const {
    return const_cast<TensorRef<T,dimensions...>*>(this)->operator()(index...);
  }

 private:
  inline TensorRef(T *data) : data(data) {}
  T *data;

  friend class FieldRefBaseParameterized<T, dimensions...>;
};

template <typename T, int... dims>
std::ostream &operator<<(std::ostream &os, const TensorRef<T, dims...> & t) {
  ierror << "General tensor operator<< not yet supported";
  return os;
}

template <typename T, int size>
std::ostream &operator<<(std::ostream &os, const TensorRef<T, size> &t) {
  os << "[";
  if (0 < size) {
    os << t(0);
  }

  for (int i=1; i<size; ++i) {
    os << ", " << t(i);
  }
  return os << "]";
}

template <typename T, int r, int c>
std::ostream &operator<<(std::ostream &os, const TensorRef<T, r, c> &t) {
  os << "[";
  if (0 < r) {
    if (0 < c) {
      os << t(0,0);
    }
    for (int j=1; j<c; ++j) {
      os << ", " << t(0,j);
    }
  }

  for (int i=0; i<r; ++i) {
    os << "; ";
    if (0 < c) {
      os << t(i,0);
    }
    for (int j=1; j<c; ++j) {
      os << ", " << t(i,j);
    }
  }
  return os << "]";
}

// Graph generators
void createElements(Set<> *elements, unsigned num);

class Box {
public:
  typedef std::pair<ElementRef,ElementRef> Coord;

  Box(unsigned nX, unsigned nY, unsigned nZ, std::vector<ElementRef> refs,
      std::map<Box::Coord, ElementRef> coords2edges)
      : nX(nX), nY(nY), nZ(nZ), refs(refs), coords2edges(coords2edges) {
    iassert(refs.size() == nX*nY*nZ);
  }

  unsigned numX() const {return nX;}
  unsigned numY() const {return nY;}
  unsigned numZ() const {return nZ;}

  ElementRef operator()(unsigned x, unsigned y, unsigned z) {
    return refs[z*nY*nX + y*nX + x];
  }

  ElementRef getEdge(ElementRef p1, ElementRef p2) const {
    Coord coord(p1,p2);
    if (coords2edges.find(coord) == coords2edges.end()) {
      return ElementRef();
    }
    return coords2edges.at(coord);
  }

  std::vector<ElementRef> getEdges() {
    std::vector<ElementRef> edges;
    for (auto &coord2edge : coords2edges) {
      edges.push_back(coord2edge.second);
    }
    return edges;
  }

private:
  unsigned nX, nY, nZ;
  std::vector<ElementRef> refs;
  std::map<Coord, ElementRef> coords2edges;
};

Box createBox(Set<> *elements, Set<2> *edges,
              unsigned numX, unsigned numY, unsigned numZ);


} // namespace simit

#endif
