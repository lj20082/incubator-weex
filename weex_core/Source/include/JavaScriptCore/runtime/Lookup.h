/*
 *  Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 *  Copyright (C) 2003, 2006, 2007, 2008, 2009, 2016 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#pragma once

#include "BatchedTransitionOptimizer.h"
#include "CallFrame.h"
#include "CustomGetterSetter.h"
#include "DOMJITGetterSetter.h"
#include "DOMJITSignature.h"
#include "Identifier.h"
#include "IdentifierInlines.h"
#include "Intrinsic.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "LazyProperty.h"
#include "PropertySlot.h"
#include "PutPropertySlot.h"
#include "TypeError.h"
#include <wtf/Assertions.h>

namespace JSC {

struct CompactHashIndex {
    const int16_t value;
    const int16_t next;
};

// FIXME: There is no reason this get function can't be simpler.
// ie. typedef JSValue (*GetFunction)(ExecState*, JSObject* baseObject)
typedef PropertySlot::GetValueFunc GetFunction;
typedef PutPropertySlot::PutValueFunc PutFunction;
typedef FunctionExecutable* (*BuiltinGenerator)(VM&);
typedef JSValue (*LazyPropertyCallback)(VM&, JSObject*);
typedef DOMJIT::GetterSetter* (*DOMJITGetterSetterGenerator)(void);

// Hash table generated by the create_hash_table script.
struct HashTableValue {
    const char* m_key; // property name
    unsigned m_attributes; // JSObject attributes
    Intrinsic m_intrinsic;
    union ValueStorage {
        constexpr ValueStorage(intptr_t value1, intptr_t value2)
            : value1(value1)
            , value2(value2)
        { }
        constexpr ValueStorage(long long constant)
            : constant(constant)
        { }

        struct {
            intptr_t value1;
            intptr_t value2;
        };
        long long constant;
    } m_values;

    unsigned attributes() const { return m_attributes; }

    Intrinsic intrinsic() const { ASSERT(m_attributes & Function); return m_intrinsic; }
    BuiltinGenerator builtinGenerator() const { ASSERT(m_attributes & Builtin); return reinterpret_cast<BuiltinGenerator>(m_values.value1); }
    NativeFunction function() const { ASSERT(m_attributes & Function); return reinterpret_cast<NativeFunction>(m_values.value1); }
    unsigned char functionLength() const
    {
        ASSERT(m_attributes & Function);
        if (m_attributes & DOMJITFunction)
            return signature()->argumentCount;
        return static_cast<unsigned char>(m_values.value2);
    }

    GetFunction propertyGetter() const { ASSERT(!(m_attributes & BuiltinOrFunctionOrAccessorOrLazyPropertyOrConstant)); return reinterpret_cast<GetFunction>(m_values.value1); }
    PutFunction propertyPutter() const { ASSERT(!(m_attributes & BuiltinOrFunctionOrAccessorOrLazyPropertyOrConstant)); return reinterpret_cast<PutFunction>(m_values.value2); }

    DOMJIT::GetterSetter* domJIT() const { ASSERT(m_attributes & DOMJITAttribute); return reinterpret_cast<DOMJITGetterSetterGenerator>(m_values.value1)(); }
    const DOMJIT::Signature* signature() const { ASSERT(m_attributes & DOMJITFunction); return reinterpret_cast<const DOMJIT::Signature*>(m_values.value2); }

    NativeFunction accessorGetter() const { ASSERT(m_attributes & Accessor); return reinterpret_cast<NativeFunction>(m_values.value1); }
    NativeFunction accessorSetter() const { ASSERT(m_attributes & Accessor); return reinterpret_cast<NativeFunction>(m_values.value2); }
    BuiltinGenerator builtinAccessorGetterGenerator() const;
    BuiltinGenerator builtinAccessorSetterGenerator() const;

    long long constantInteger() const { ASSERT(m_attributes & ConstantInteger); return m_values.constant; }

    intptr_t lexerValue() const { ASSERT(!m_attributes); return m_values.value1; }
    
    ptrdiff_t lazyCellPropertyOffset() const { ASSERT(m_attributes & CellProperty); return m_values.value1; }
    ptrdiff_t lazyClassStructureOffset() const { ASSERT(m_attributes & ClassStructure); return m_values.value1; }
    LazyPropertyCallback lazyPropertyCallback() const { ASSERT(m_attributes & PropertyCallback); return reinterpret_cast<LazyPropertyCallback>(m_values.value1); }
};

struct HashTable {
    int numberOfValues;
    int indexMask;
    bool hasSetterOrReadonlyProperties;

    const HashTableValue* values; // Fixed values generated by script.
    const CompactHashIndex* index;

    // Find an entry in the table, and return the entry.
    ALWAYS_INLINE const HashTableValue* entry(PropertyName propertyName) const
    {
        if (propertyName.isSymbol())
            return nullptr;

        auto uid = propertyName.uid();
        if (!uid)
            return nullptr;

        int indexEntry = IdentifierRepHash::hash(uid) & indexMask;
        int valueIndex = index[indexEntry].value;
        if (valueIndex == -1)
            return nullptr;

        while (true) {
            if (WTF::equal(uid, values[valueIndex].m_key))
                return &values[valueIndex];

            indexEntry = index[indexEntry].next;
            if (indexEntry == -1)
                return nullptr;
            valueIndex = index[indexEntry].value;
            ASSERT(valueIndex != -1);
        };
    }

    class ConstIterator {
    public:
        ConstIterator(const HashTable* table, int position)
            : m_table(table)
            , m_position(position)
        {
            skipInvalidKeys();
        }

        const HashTableValue* value() const
        {
            return &m_table->values[m_position];
        }

        const HashTableValue& operator*() const { return *value(); }

        const char* key() const
        {
            return m_table->values[m_position].m_key;
        }

        const HashTableValue* operator->() const
        {
            return value();
        }

        bool operator!=(const ConstIterator& other) const
        {
            ASSERT(m_table == other.m_table);
            return m_position != other.m_position;
        }

        ConstIterator& operator++()
        {
            ASSERT(m_position < m_table->numberOfValues);
            ++m_position;
            skipInvalidKeys();
            return *this;
        }

    private:
        void skipInvalidKeys()
        {
            ASSERT(m_position <= m_table->numberOfValues);
            while (m_position < m_table->numberOfValues && !m_table->values[m_position].m_key)
                ++m_position;
            ASSERT(m_position <= m_table->numberOfValues);
        }

        const HashTable* m_table;
        int m_position;
    };

    ConstIterator begin() const
    {
        return ConstIterator(this, 0);
    }
    ConstIterator end() const
    {
        return ConstIterator(this, numberOfValues);
    }
};

JS_EXPORT_PRIVATE bool setUpStaticFunctionSlot(VM&, const HashTableValue*, JSObject* thisObject, PropertyName, PropertySlot&);
JS_EXPORT_PRIVATE void reifyStaticAccessor(VM&, const HashTableValue&, JSObject& thisObject, PropertyName);

inline BuiltinGenerator HashTableValue::builtinAccessorGetterGenerator() const
{
    ASSERT(m_attributes & Accessor);
    ASSERT(m_attributes & Builtin);
    return reinterpret_cast<BuiltinGenerator>(m_values.value1);
}

inline BuiltinGenerator HashTableValue::builtinAccessorSetterGenerator() const
{
    ASSERT(m_attributes & Accessor);
    ASSERT(m_attributes & Builtin);
    return reinterpret_cast<BuiltinGenerator>(m_values.value2);
}

inline bool getStaticPropertySlotFromTable(VM& vm, const HashTable& table, JSObject* thisObject, PropertyName propertyName, PropertySlot& slot)
{
    if (thisObject->staticPropertiesReified())
        return false;

    auto* entry = table.entry(propertyName);
    if (!entry)
        return false;

    if (entry->attributes() & BuiltinOrFunctionOrAccessorOrLazyProperty)
        return setUpStaticFunctionSlot(vm, entry, thisObject, propertyName, slot);

    if (entry->attributes() & ConstantInteger) {
        slot.setValue(thisObject, attributesForStructure(entry->attributes()), jsNumber(entry->constantInteger()));
        return true;
    }

    if (entry->attributes() & DOMJITAttribute) {
        DOMJIT::GetterSetter* domJIT = entry->domJIT();
        slot.setCacheableCustom(thisObject, attributesForStructure(entry->attributes()), domJIT->getter(), domJIT);
        return true;
    }

    slot.setCacheableCustom(thisObject, attributesForStructure(entry->attributes()), entry->propertyGetter());
    return true;
}

inline bool replaceStaticPropertySlot(VM& vm, JSObject* thisObject, PropertyName propertyName, JSValue value)
{
    if (!thisObject->putDirect(vm, propertyName, value))
        return false;

    if (!thisObject->staticPropertiesReified())
        thisObject->JSObject::setStructure(vm, Structure::attributeChangeTransition(vm, thisObject->structure(), propertyName, 0));

    return true;
}

// 'base' means the object holding the property (possibly in the prototype chain of the object put was called on).
// 'thisValue' is the object that put is being applied to (in the case of a proxy, the proxy target).
// 'slot.thisValue()' is the object the put was originally performed on (in the case of a proxy, the proxy itself).
inline bool putEntry(ExecState* exec, const HashTableValue* entry, JSObject* base, JSObject* thisValue, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    VM& vm = exec->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (entry->attributes() & BuiltinOrFunctionOrLazyProperty) {
        if (!(entry->attributes() & ReadOnly)) {
            // If this is a function or lazy property put then we just do the put because
            // logically the object already had the property, so this is just a replace.
            if (JSObject* thisObject = jsDynamicCast<JSObject*>(vm, thisValue))
                thisObject->putDirect(vm, propertyName, value);
            return true;
        }
        return typeError(exec, scope, slot.isStrictMode(), ASCIILiteral(ReadonlyPropertyWriteError));
    }

    if (entry->attributes() & Accessor)
        return typeError(exec, scope, slot.isStrictMode(), ASCIILiteral(ReadonlyPropertyWriteError));

    if (!(entry->attributes() & ReadOnly)) {
        ASSERT_WITH_MESSAGE(!(entry->attributes() & DOMJITAttribute), "DOMJITAttribute supports readonly attributes currently.");
        bool isAccessor = entry->attributes() & CustomAccessor;
        JSValue updateThisValue = entry->attributes() & CustomAccessor ? slot.thisValue() : JSValue(base);
        bool result = callCustomSetter(exec, entry->propertyPutter(), isAccessor, updateThisValue, value);
        if (isAccessor)
            slot.setCustomAccessor(base, entry->propertyPutter());
        else
            slot.setCustomValue(base, entry->propertyPutter());
        return result;
    }

    return typeError(exec, scope, slot.isStrictMode(), ASCIILiteral(ReadonlyPropertyWriteError));
}

/**
 * This one is for "put".
 * It looks up a hash entry for the property to be set.  If an entry
 * is found it sets the value and returns true, else it returns false.
 */
inline bool lookupPut(ExecState* exec, PropertyName propertyName, JSObject* base, JSValue value, const HashTable& table, PutPropertySlot& slot, bool& putResult)
{
    const HashTableValue* entry = table.entry(propertyName);

    if (!entry)
        return false;

    putResult = putEntry(exec, entry, base, base, propertyName, value, slot);
    return true;
}

inline void reifyStaticProperty(VM& vm, const PropertyName& propertyName, const HashTableValue& value, JSObject& thisObj)
{
    if (value.attributes() & Builtin) {
        if (value.attributes() & Accessor)
            reifyStaticAccessor(vm, value, thisObj, propertyName);
        else
            thisObj.putDirectBuiltinFunction(vm, thisObj.globalObject(), propertyName, value.builtinGenerator()(vm), attributesForStructure(value.attributes()));
        return;
    }

    if (value.attributes() & Function) {
        if (value.attributes() & DOMJITFunction) {
            thisObj.putDirectNativeFunction(
                vm, thisObj.globalObject(), propertyName, value.functionLength(),
                value.function(), value.intrinsic(), value.signature(), attributesForStructure(value.attributes()));
            return;
        }
        thisObj.putDirectNativeFunction(
            vm, thisObj.globalObject(), propertyName, value.functionLength(),
            value.function(), value.intrinsic(), attributesForStructure(value.attributes()));
        return;
    }

    if (value.attributes() & ConstantInteger) {
        thisObj.putDirect(vm, propertyName, jsNumber(value.constantInteger()), attributesForStructure(value.attributes()));
        return;
    }

    if (value.attributes() & Accessor) {
        reifyStaticAccessor(vm, value, thisObj, propertyName);
        return;
    }
    
    if (value.attributes() & CellProperty) {
        LazyCellProperty* property = bitwise_cast<LazyCellProperty*>(
            bitwise_cast<char*>(&thisObj) + value.lazyCellPropertyOffset());
        JSCell* result = property->get(&thisObj);
        thisObj.putDirect(vm, propertyName, result, attributesForStructure(value.attributes()));
        return;
    }
    
    if (value.attributes() & ClassStructure) {
        LazyClassStructure* structure = bitwise_cast<LazyClassStructure*>(
            bitwise_cast<char*>(&thisObj) + value.lazyClassStructureOffset());
        structure->get(jsCast<JSGlobalObject*>(&thisObj));
        return;
    }
    
    if (value.attributes() & PropertyCallback) {
        JSValue result = value.lazyPropertyCallback()(vm, &thisObj);
        thisObj.putDirect(vm, propertyName, result, attributesForStructure(value.attributes()));
        return;
    }

    if (value.attributes() & DOMJITAttribute) {
        DOMJIT::GetterSetter* domJIT = value.domJIT();
        CustomGetterSetter* customGetterSetter = CustomGetterSetter::create(vm, domJIT->getter(), domJIT->setter(), domJIT);
        thisObj.putDirectCustomAccessor(vm, propertyName, customGetterSetter, attributesForStructure(value.attributes()));
        return;
    }

    CustomGetterSetter* customGetterSetter = CustomGetterSetter::create(vm, value.propertyGetter(), value.propertyPutter());
    thisObj.putDirectCustomAccessor(vm, propertyName, customGetterSetter, attributesForStructure(value.attributes()));
}

template<unsigned numberOfValues>
inline void reifyStaticProperties(VM& vm, const HashTableValue (&values)[numberOfValues], JSObject& thisObj)
{
    BatchedTransitionOptimizer transitionOptimizer(vm, &thisObj);
    for (auto& value : values) {
        if (!value.m_key)
            continue;
        auto key = Identifier::fromString(&vm, reinterpret_cast<const LChar*>(value.m_key), strlen(value.m_key));
        reifyStaticProperty(vm, key, value, thisObj);
    }
}

template<NativeFunction nativeFunction, int length> EncodedJSValue nonCachingStaticFunctionGetter(ExecState* state, EncodedJSValue, PropertyName propertyName)
{
    return JSValue::encode(JSFunction::create(state->vm(), state->lexicalGlobalObject(), length, propertyName.publicName(), nativeFunction));
}

} // namespace JSC
