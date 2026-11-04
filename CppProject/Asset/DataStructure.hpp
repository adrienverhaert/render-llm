#pragma once
#include "Asset.hpp"
#include "Heap.hpp"
#include "Type/VarType.hpp"
#include "Type/ArrType.hpp"
#include "Type/VecType.hpp"

namespace CppProject
{
	// Map data structure
	struct Map : Asset
	{
		Map(IntType subAssetId = 0) : Asset(ID_Map, subAssetId) {};
		virtual ~Map();

		struct MapValue
		{
			VarType value;
			IntType dsType = 0;
		};

		virtual VarType& operator[](const VarType& key) { return map[key].value; } // map[key] = ..

		// Returns a value at the given key.
		virtual VarType Value(const VarType& key) const;
		virtual VarType Value(IntType key) const { return Value(VarType(key)); };
		virtual VarType Value(const char* key) const { return Value(VarType(key)); };

		// Returns the DATASTRUCTURE TYPE stored next to the value at a key
		// (ds_type_map, ds_type_list, or 0 for a plain value).
		//
		// WHY THIS EXISTS. A map id and a list id are plain integers in GML,
		// so once a JSON document is parsed a nested {} / [] and a number are
		// the same thing to is_real(). MEASURED on the build ShinyTrims O11
		// shipped (o12\evidencia\antes_o12.txt): item.set_trim
		// {"intensity":[3]} answered "intensity fuera de rango: 188940" and
		// {"intensity":{"v":3}} answered "188946" -- those are the LIST and
		// MAP IDS, read as if they were the intensity the caller typed. An id
		// landing in 0..4 would have passed the range check outright. In the
		// other direction ds_map_valid(7) answers TRUE if a map with id 7
		// happens to exist, which depends on how the process started up.
		// The mark that json_load_object() already writes (ds_map_add_map /
		// ds_map_add_list, DataStructureFunc.cpp) is the only answer that does
		// not depend on luck, and it lived here with no way to ask for it from
		// GML. ds_map_is_map / ds_map_is_list are that question.
		virtual IntType DsType(const VarType& key) const { return map.value(key).dsType; }

		// Sets the value and datastructure type of a key.
		virtual void Set(const VarType& key, MapValue value) { map[key] = value; }

		// Removes the value at a key.
		virtual void Remove(const VarType& key) { map.remove(key); }

		// Returns whether the map contains a key.
		virtual BoolType Contains(const VarType& key) const { return map.contains(key); }

		// Copy the values in the map to another.
		virtual void Copy(Map* to) const;

		// Finds the first key.
		virtual VarType GetFirstKey();

		// Finds the next key.
		virtual VarType GetNextKey(const VarType& key);

		// Returns the size of the map.
		virtual IntType GetSize() { return map.size(); }

		// Clear map keys and values and recursively destroys data structures.
		virtual void Clear();

		// Debug map key/value mappings.
		virtual void Debug() const;

		QMap<VarType, MapValue> map;
		QMapIterator<VarType, MapValue>* mapIt = nullptr;

		enum Type
		{
			MAP,
			HASH_INT,
			HASH_STRING,
		};
		virtual Type GetType() const { return MAP; }
	};

	// Map data structure with a hash table for faster lookup
	template <typename T> struct HashMap : Map
	{
		using Map::Map;
		~HashMap();
		VarType& operator[](const VarType& key) override { return hash[(T)key].value; }
		VarType Value(const VarType& key) const override { return hash.value((T)key).value; }
		IntType DsType(const VarType& key) const override { return hash.value((T)key).dsType; }
		void Set(const VarType& key, MapValue value) override { hash[(T)key] = value; }
		void Remove(const VarType& key) override { hash.remove((T)key); }
		BoolType Contains(const VarType& key) const override { return hash.contains(key); }
		void Copy(Map* to) const override;
		VarType GetFirstKey() override;
		VarType GetNextKey(const VarType& key) override;
		IntType GetSize() override { return hash.size(); }
		void Clear() override;
		void Debug() const override;

		QHash<T, MapValue> hash;
		QHashIterator<T, MapValue>* hashIt = nullptr;
	};

	struct IntHashMap : HashMap<IntType>
	{
		IntHashMap() : HashMap(ID_IntHashMap) {}
		VarType Value(IntType key) const override { return hash.value(key).value; }
		Type GetType() const override { return HASH_INT; }
	};
	struct StringHashMap : HashMap<StringType>
	{
		StringHashMap() : HashMap(ID_StringHashMap) {}
		VarType Value(const char* key) const override { return hash.value(key).value; }
		Type GetType() const override { return HASH_STRING; }
	};

	// List data structure
	// Each element is a struct with { value, dsType }
	struct List : Asset
	{
		List() : Asset(ID_List) {};
		~List();

		VarType& operator[](IntType); // list[index] = value

		// Returns the value at an index.
		const VarType& Value(IntType index) const;

		// Clear the list and recursively destroys data structures
		void Clear();

		// Debug list contents
		void Debug() const;

		struct ListValue
		{
			VarType value;
			IntType dsType = 0;
		};
		QVector<ListValue> vec;
	};

	// Stack data structure
	struct Stack : Asset
	{
		Stack() : Asset(ID_Stack) {};

		QStack<VarType> stack;
	};

	// Priority queue data structure
	// Each value is a struct with { value, priority }
	struct Priority : Asset
	{
		Priority() : Asset(ID_Priority) {};

		IntType GetMaxPrioIndex() const;

		struct PriorityValue
		{
			VarType value;
			IntType prio = 0;
		};
		QVector<PriorityValue> vec;
	};
}