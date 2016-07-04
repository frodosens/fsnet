//
//  FSParams.h
//  fsnet_client
//
//  Created by Vincent on 15/5/5.
//  Copyright (c) 2015年 Vincent. All rights reserved.
//

#ifndef __fsnet_client__FSParams__
#define __fsnet_client__FSParams__

#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "fs_stream.h"
#include "fs_define.h"
#include "fs_malloc.h"
#include "FSObject.h"





typedef enum {
    
    PARAMS_TYPE_INT,
    PARAMS_TYPE_DOUBLE,
    PARAMS_TYPE_UTF8,
    PARAMS_TYPE_ARRAY,
    PARAMS_TYPE_DOUCMENT,
    PARAMS_TYPE_BOOL,
    PARAMS_TYPE_INT64,
    PARAMS_TYPE_NULL,
    
    
    PARAMS_TYPE_MAX
    
} params_type;

class FSParams;
class FSParamsBase : public FSObject{
    
    friend class FSParams;
    
private:
    char key[64];
protected:
    params_type type;
public:
    
    virtual ~FSParamsBase(){}
    
    
    void setType(params_type type){
        this->type = type;
    }
    params_type getType(){
        return this->type;
    }
    void setKey(const char* key){
        strncpy(this->key, key, 64);
    }
    const char* getKey() const{
        return this->key;
    }
    
    
    virtual FSParamsBase* copy(){
        return this;
    };
};

template<class T>
class FSParamsObject : public FSParamsBase{
    friend class FSParams;
protected:
    T val;
public:
    FSParamsObject(const FSParamsObject& copy){
        setKey(copy.getKey());
        this->val = copy.val;
        this->type = copy.type;
    }
    FSParamsObject(const T& v){
        this->val = v;
    }
    virtual ~FSParamsObject(){}
    virtual operator T() const{
        return val;
    }
    virtual T Get(){
        return val;
    }
    
    virtual FSParamsBase* copy(){
        
        FSParamsObject<T> *ret = new FSParamsObject<T>(val);
        ret->setKey(this->getKey());
        ret->setType(this->type);
        return ret;
        
    };
};

class FSParamsString : public FSParamsObject<const char*>{
    
private:
    char* m_data;
    uint32_t m_len;
public:
    FSParamsString(const char* val, uint32_t len);
    FSParamsString(const FSParamsString&);
    ~FSParamsString();
#define PARAMS_TYPE_CONVER( TYPE, FUN ) \
virtual operator TYPE() const{ \
return FUN(m_data); \
}
    PARAMS_TYPE_CONVER(int, atoi);
    PARAMS_TYPE_CONVER(short, atoi);
    PARAMS_TYPE_CONVER(unsigned int, atoi);
    PARAMS_TYPE_CONVER(float, atof);
    PARAMS_TYPE_CONVER(double, atof);
    PARAMS_TYPE_CONVER(long, atol);
    PARAMS_TYPE_CONVER(long long, atoll);
    PARAMS_TYPE_CONVER(unsigned long long, atoll);
    
    virtual FSParamsBase* copy(){
        
        FSParamsString *ret = new FSParamsString(m_data, m_len);
        ret->setKey(this->getKey());
        ret->setType(this->type);
        return ret;
        
    };
};


class FSParamsDocument : public FSParamsObject<FSParams*>{
    
    friend class FSParams;
protected:
    FSParams* m_data;
public:
    FSParamsDocument( FSParams* );
    FSParamsDocument(const FSParamsDocument&);
    ~FSParamsDocument();
    
    
    virtual FSParamsDocument* copy(){
        
        FSParamsDocument *ret = new FSParamsDocument( m_data );
        ret->setKey(this->getKey());
        ret->setType(this->type);
        return ret;
        
    };
};



#define INLINE inline
class FSParams{
    
public:
    typedef std::vector<FSParamsBase*> RPCMethodParamsList;
    
    typedef FSParams::RPCMethodParamsList::const_iterator params_iterate;
    
private:
    RPCMethodParamsList m_params_list;
protected:
    
    fs_bool type_array;
    fs_bool type_hash;
public:
    
    FSParams();
    FSParams(const FSParams& );
    ~FSParams();
    
    
protected:
    
    template<class T>
    INLINE const FSParamsObject<T>* getObj(const char* key)  const{
        
        params_iterate iter = m_params_list.begin();
        for( ; iter != m_params_list.end() ; iter++){
            if(strcmp((*iter)->getKey(), key) == 0){
                FSParamsObject<T>* ret = (FSParamsObject<T>*)(*iter);
                return ret;
            }
        }
        return NULL;
    }
    
    
public:
    
    INLINE void add(FSParamsBase* val){
        m_params_list.push_back(val);
    }
    
    INLINE bool exist(const char* key) const{
        return getObj<void*>(key) != NULL;
    }
    
    template<class T>
    INLINE T get(int index) const{
        char key[12];
        sprintf(key, "%d", index);
        const FSParamsObject<T> *ret = getObj<T>(key);
        if(ret == NULL) return NULL;
        return (*ret);
    }
    
    template<class T>
    INLINE T get(const char* key) const{
        const FSParamsObject<T> *ret = getObj<T>(key);
        if(ret == NULL) return NULL;
        return (*ret);
    }
    
    template<class T>
    INLINE void push(T val, params_type type=PARAMS_TYPE_UTF8){
        fs_assert(type_array, "type == hash can' not call push");
        char key[32];
        sprintf(key, "%d", size());
        set(key, val, type);
    }
    
    template<class T>
    INLINE void set(const char* key, T val, params_type type=PARAMS_TYPE_UTF8){
        FSParamsObject<T>* valObj = new FSParamsObject<T>(val);
        valObj->setType(type);
        valObj->setKey(key);
        add(valObj);
    }
    
    INLINE void set(const char* key, void* data, uint32_t len, params_type type=PARAMS_TYPE_UTF8){
        FSParamsString* valObj = new FSParamsString((const char*)data, len);
        valObj->setKey(key);
        valObj->setType(type);
        add(valObj);
    }
    
    INLINE void set(const char* key, FSParams& val, params_type type=PARAMS_TYPE_DOUCMENT){
        FSParamsDocument* valObj = new FSParamsDocument(&val);
        valObj->setKey(key);
        valObj->setType(type);
        add(valObj);
    }
    
    
    INLINE void setUTF8(const char* key, const char* str){
        set(key, (void*)str, (uint32_t)strlen(str));
    }
    
    
#define MB_PARAMS_SET(TYPE1, TYPE2, TYPE3) \
void set##TYPE1(const char* key, TYPE2 val){ \
set(key, val, TYPE3); \
}\
void set(const char* key, TYPE2 val){ \
set(key, val, TYPE3); \
}\
void push(TYPE2 val){ \
push(val, TYPE3); \
}
    
    
    MB_PARAMS_SET(Int, int, PARAMS_TYPE_INT);
    MB_PARAMS_SET(Int64, long long, PARAMS_TYPE_INT64);
    MB_PARAMS_SET(Bool, bool, PARAMS_TYPE_BOOL);
    MB_PARAMS_SET(Double, double, PARAMS_TYPE_DOUBLE);
    MB_PARAMS_SET(Array, FSParams*, PARAMS_TYPE_ARRAY);
    
    params_iterate begin() const;
    params_iterate end() const;

    int size() const;
    
    void write_to_stream(struct fs_output_stream*, fs_bool write_head=true);
    void init_from_stream(struct fs_input_stream*, fs_bool check_head=true);
    
    void debug_print(int deep=0) const;
    
};


class FSParamsArray : public FSParams{
public:
    FSParamsArray():FSParams(){
        type_array = fs_true;
        type_hash = fs_false;
    }
    
    FSParamsArray(const FSParamsArray& other ):FSParams(other){
        type_array = fs_true;
        type_hash = fs_false;
    }
    
};

typedef FSParams FSParamsHash;


#endif /* defined(__fsnet_client__FSParams__) */
