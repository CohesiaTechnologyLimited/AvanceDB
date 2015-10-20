/*
 *  AvanceDB - an in-memory database similar to Apache CouchDB
 *  Copyright (C) 2015 Ripcord Software
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as
 *  published by the Free Software Foundation, either version 3 of the
 *  License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "map_reduce.h"

#include <boost/make_shared.hpp>
#include <boost/scope_exit.hpp>

#include <memory>
#include <algorithm>

#include "script_array_jsapi_key_value_source.h"
#include "map_reduce_result.h"
#include "script_object_jsapi_source.h"
#include "script_array_jsapi_source.h"
#include "map_reduce_script_object_state.h"
#include "documents.h"
#include "config.h"
#include "set_thread_name.h"
#include "map_reduce_thread_pool.h"

#include "script_object_factory.h"
#include "script_array_factory.h"

MapReduce::MapReduce() : mapReduceThreadPool_(MapReduceThreadPool::Get()) {
    
}

map_reduce_results_ptr MapReduce::Execute(const GetViewOptions& options, const MapReduceTask& task, document_collections_ptr colls) {        
    std::mutex m;
    auto collsSize = colls->size();
    std::vector<map_reduce_result_array_ptr> resultArray;
    std::atomic<int> threads(collsSize);
    
    // run the map
    for (auto& docs : *colls) {
        mapReduceThreadPool_->Post([&]() {
            BOOST_SCOPE_EXIT(&threads) { --threads; } BOOST_SCOPE_EXIT_END
            auto& rt = mapReduceThreadPool_->GetThreadRuntime();

            auto result = Execute(rt, task, docs);
            
            std::unique_lock<std::mutex> l(m);
            resultArray.emplace_back(result);
        });
    }
    
    // wait for the map threads to finish
    while (threads.load() > 0) {        
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    // calculate the number of map rows and offsets
    decltype(resultArray.size()) totalRows = 0;
    std::vector<decltype(totalRows)> rowOffsets;
    for (const auto& result : resultArray) {
        rowOffsets.emplace_back(totalRows);
        totalRows += result->size();        
    }
    rowOffsets.emplace_back(totalRows);
    
    // allocate the results collection
    auto results = boost::make_shared<map_reduce_result_array_ptr::element_type>();
    results->reserve(totalRows);
    
    // copy the shard results into the main results collection
    for (const auto& result : resultArray) {
        results->insert(results->end(), result->cbegin(), result->cend());
    }
    
    auto less = [](const map_reduce_result_ptr& a, const map_reduce_result_ptr& b) {
        return MapReduceResult::Less(a, b);
    };
    
    auto mergeResultsWorker = [&](decltype(collsSize) startIndex, decltype(collsSize) step) {
        BOOST_SCOPE_EXIT(&threads) { --threads; } BOOST_SCOPE_EXIT_END
        
        auto midStep = step / 2;
        auto midIndex = startIndex + midStep;
        auto endIndex = std::min(startIndex + step, collsSize);

        auto startOffset = rowOffsets[startIndex];
        auto midOffset = rowOffsets[midIndex];
        auto endOffset = rowOffsets[endIndex];

        const auto& begin = results->begin();
        std::inplace_merge(begin + startOffset, begin + midOffset, begin + endOffset, less);
    };

    // merge the result shards on the main results collection
    decltype(collsSize) step = 2;
    while ((threads = collsSize / step) > 0) {
        
        for (decltype(collsSize) i = 0; i <= collsSize - step; i += step) {
            mapReduceThreadPool_->Post([=]() { mergeResultsWorker(i, step); });
        }
        
        // wait for the merge threads to finish
        while (threads.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        step *= 2;
    }
    
    // if we had an odd number of collections then there is one left to merge
    if ((collsSize % step) > 0) {
        mergeResultsWorker(0, step);
    }
    
    return boost::make_shared<map_reduce_results_ptr::element_type>(options, results);
}

map_reduce_result_array_ptr MapReduce::Execute(rs::jsapi::Runtime& rt, const MapReduceTask& task, const document_array& docs) {
    map_reduce_result_array_ptr results = boost::make_shared<map_reduce_result_array_ptr::element_type>();
    
    // create the function script
    std::string mapScript = "(function() { return ";
    mapScript += task.Map();
    mapScript += "; })();";
    
    document_ptr doc = nullptr;

    // define a function in global scope implemented by a C++ lambda
    rs::jsapi::Global::DefineFunction(rt, "emit", 
        [&](const std::vector<rs::jsapi::Value>& args, rs::jsapi::Value&) {
            auto source = ScriptArrayJsapiKeyValueSource::Create(args[0], args[1]);
            
            auto resultArr = rs::scriptobject::ScriptArrayFactory::CreateArray(source);
            auto result = MapReduceResult::Create(resultArr, doc);
            results->emplace_back(result);
    });

    // TODO: elegantly handle JS syntax errors
    // execute the script in the context of the runtime, getting the resulting function
    rs::jsapi::Value func(rt);
    rt.Evaluate(mapScript.c_str(), func);
    
    auto state = new MapReduceScriptObjectState{nullptr};

    rs::scriptobject::ScriptObjectPtr scriptObj = nullptr;
    rs::jsapi::Value object(rt);
    rs::jsapi::DynamicObject::Create(rt, 
        [&](const char* name, rs::jsapi::Value& value) {
            return MapReduce::GetFieldValue(scriptObj, name, value);
        }, 
        nullptr, 
        [&](std::vector<std::string>& props, std::vector<std::pair<std::string, JSNative>>&) {
            // TODO: only enumerate when really needed
            for (decltype(scriptObj->getCount()) i = 0, count = scriptObj->getCount(); i < count; ++i) {
                props.emplace_back(scriptObj->getName(i));
            }
            return true;
        }, 
        [state]() { delete state; },
        object);

    rs::jsapi::DynamicObject::SetPrivate(object, 0, state);

    rs::jsapi::FunctionArguments args(rt);
    args.Append(object);

    for (DocumentsCollection::size_type i = 0, size = docs.size(); i < size; ++i) {
        doc = docs[i];
        scriptObj = doc->getObject();

        state->scriptObj_ = scriptObj;

        // TODO: handle exception cases here
        func.CallFunction(args);
    }
    
    SortResultArray(results);   
    
    return results;
}

void MapReduce::GetFieldValue(script_object_ptr scriptObj, const char* name, rs::jsapi::Value& value) {
    int index = 0;
    switch (scriptObj->getType(name, index)) {
        case rs::scriptobject::ScriptObjectType::Boolean:
            value = scriptObj->getBoolean(index);
            return;
        case rs::scriptobject::ScriptObjectType::Int32:
            value = scriptObj->getInt32(index);
            return;
        case rs::scriptobject::ScriptObjectType::String:
            value = scriptObj->getString(index);
            return;
        case rs::scriptobject::ScriptObjectType::Double:
            value = scriptObj->getDouble(index);
            return;
        case rs::scriptobject::ScriptObjectType::Object: {
            auto childObj = scriptObj->getObject(index);
            MapReduce::CreateValueObject(childObj, value);
            return;
        }
        case rs::scriptobject::ScriptObjectType::Array: {
            auto childArr = scriptObj->getArray(index);
            MapReduce::CreateValueArray(childArr, value);
            return;
        }
        case rs::scriptobject::ScriptObjectType::Null:
            value = JS::NullHandleValue;
            return;
        default:
            value = JS::UndefinedHandleValue;
            return;
    }
}

void MapReduce::GetFieldValue(script_array_ptr scriptArr, int index, rs::jsapi::Value& value) {
    switch (scriptArr->getType(index)) {
        case rs::scriptobject::ScriptObjectType::Boolean:
            value = scriptArr->getBoolean(index);
            return;
        case rs::scriptobject::ScriptObjectType::Int32:
            value = scriptArr->getInt32(index);
            return;
        case rs::scriptobject::ScriptObjectType::String:
            value = scriptArr->getString(index);
            return;
        case rs::scriptobject::ScriptObjectType::Double:
            value = scriptArr->getDouble(index);
            return;
        case rs::scriptobject::ScriptObjectType::Object: {
            auto childObj = scriptArr->getObject(index);
            MapReduce::CreateValueObject(childObj, value);
            return;
        }
        case rs::scriptobject::ScriptObjectType::Array: {
            auto childArr = scriptArr->getArray(index);
            MapReduce::CreateValueArray(childArr, value);
            return;
        }
        case rs::scriptobject::ScriptObjectType::Null:
            value = JS::NullHandleValue;
            return;
        default:
            value = JS::UndefinedHandleValue;
    }
}

void MapReduce::CreateValueObject(script_object_ptr obj, rs::jsapi::Value& value) {
    auto state = new MapReduceScriptObjectState{obj};
            
    auto cx = value.getContext();
    rs::jsapi::DynamicObject::Create(cx, 
        [state](const char* name, rs::jsapi::Value& value) {
            return MapReduce::GetFieldValue(state->scriptObj_, name, value);
        }, 
        nullptr, 
        [state](std::vector<std::string>& props, std::vector<std::pair<std::string, JSNative>>&) {
            // TODO: it would be useful to cache this for performance
            for (decltype(state->scriptObj_->getCount()) i = 0, count = state->scriptObj_->getCount(); i < count; ++i) {
                props.emplace_back(state->scriptObj_->getName(i));
            }
            return true;
        }, 
        [state]() { delete state; },
        value);

    rs::jsapi::DynamicObject::SetPrivate(value, 0, state);
}

void MapReduce::CreateValueArray(script_array_ptr arr, rs::jsapi::Value& value) {
    auto state = new MapReduceScriptArrayState{arr};
    
    auto cx = value.getContext();
    rs::jsapi::DynamicArray::Create(cx, 
        [state](int index, rs::jsapi::Value& value) {
            return MapReduce::GetFieldValue(state->scriptArray_, index, value);
        }, 
        nullptr, 
        [state]() { return state->scriptArray_->getCount(); }, 
        [state]() { delete state; },
        value);
        
    rs::jsapi::DynamicArray::SetPrivate(value, 0, state);
}

script_object_ptr MapReduce::GetValueScriptObject(const rs::jsapi::Value& obj) {
    if (rs::jsapi::DynamicObject::IsDynamicObject(obj)) {
        uint64_t value = 0;
        void* ptr = nullptr;
        rs::jsapi::DynamicObject::GetPrivate(obj, value, ptr);
        return reinterpret_cast<MapReduceScriptObjectState*>(ptr)->scriptObj_;
    }
    else {
        auto source = ScriptObjectJsapiSource::Create(obj);
        return rs::scriptobject::ScriptObjectFactory::CreateObject(source);
    }
}

script_array_ptr MapReduce::GetValueScriptArray(const rs::jsapi::Value& arr) {
    if (rs::jsapi::DynamicArray::IsDynamicArray(arr)) {
        uint64_t value = 0;
        void* ptr = nullptr;
        rs::jsapi::DynamicArray::GetPrivate(arr, value, ptr);
        return reinterpret_cast<MapReduceScriptArrayState*>(ptr)->scriptArray_;
    } else {
        auto source = ScriptArrayJsapiSource::Create(arr);
        return rs::scriptobject::ScriptArrayFactory::CreateArray(source);
    }
}

void MapReduce::SortResultArray(map_reduce_result_array_ptr results) {
    std::sort(results->begin(), results->end(), [](const map_reduce_result_ptr& a, const map_reduce_result_ptr& b) {
        return MapReduceResult::Less(a, b);
    });
}