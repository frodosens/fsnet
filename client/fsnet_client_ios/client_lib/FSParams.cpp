//
//  FSParams.cpp
//  fsnet_client
//
//  Created by Vincent on 15/5/5.
//  Copyright (c) 2015年 Vincent. All rights reserved.
//

#include "FSParams.h"
#include "fs_malloc.h"

#include <string>



FSParamsString::FSParamsString(const char* val, uint32_t len):
FSParamsObject<const char*>(NULL), m_data(NULL){
    m_len = len;
    m_data = (char*)fs_malloc(len + 1);
    this->val = m_data;
    this->type = PARAMS_TYPE_UTF8;
    memset(m_data, 0, m_len + 1);
    memcpy(m_data, val, len);
    
}
FSParamsString::FSParamsString(const FSParamsString& copy):
FSParamsObject<const char*>(NULL), m_data(NULL){
    m_len = copy.m_len;
    m_data = (char*)fs_malloc(m_len + 1);
    this->val = m_data;
    this->type = PARAMS_TYPE_UTF8;
    memset(m_data, 0, m_len + 1);
    memcpy(m_data, copy.m_data, m_len);
}

FSParamsString::~FSParamsString(){
    fs_free(m_data);
    this->val = NULL;
}



FSParamsDocument::FSParamsDocument( FSParams* params ):
FSParamsObject<FSParams*>(params){
    
    this->m_data = new FSParams;
    this->val = m_data;
    this->type = PARAMS_TYPE_DOUCMENT;
    
    FSParams::RPCMethodParamsList::const_iterator iter = params->begin();
    
    for( ; iter != params->end() ; iter++){
        FSParamsBase* params = (*iter)->copy();
        this->m_data->add(params);
    }
    
    
}

FSParamsDocument::FSParamsDocument(const FSParamsDocument& params):
FSParamsObject<FSParams*>(NULL){
    
    this->m_data = new FSParams;
    this->val = m_data;
    this->type = PARAMS_TYPE_DOUCMENT;
    
    FSParams::RPCMethodParamsList::const_iterator iter = params.m_data->begin();
    
    for( ; iter != params.m_data->end() ; iter++){
        FSParamsBase* params = (*iter)->copy();
        this->m_data->add(params);
    }
    
    
}

FSParamsDocument::~FSParamsDocument(){
    
    delete (m_data);
    this->val = NULL;
    
}



FSParams::FSParams():type_hash(fs_true),type_array(fs_false){
}


FSParams::FSParams(const FSParams& copy ):type_hash(fs_true),type_array(fs_false){
    
    
    FSParams::RPCMethodParamsList::const_iterator iter = copy.begin();
    
    for( ; iter != copy.end() ; iter++){
        FSParamsBase* params = (*iter)->copy();
        this->add(params);
    }
    
    
    
}

FSParams::~FSParams(){
    
    
    params_iterate iter;
    
    for(iter = m_params_list.begin() ; iter != m_params_list.end() ; iter++){
        FSParamsBase* params = *iter;
        delete (params);
    }
    m_params_list.clear();
    
}


FSParams::params_iterate FSParams::begin() const{
    return m_params_list.begin();
}
FSParams::params_iterate FSParams::end() const{
    return m_params_list.end();
}

int FSParams::size() const
{
    return (int)m_params_list.size();
}

void FSParams::write_to_stream(struct fs_output_stream* os, fs_bool write_head){
    
    if(write_head){
        fs_stream_write_byte(os, type_array ? PARAMS_TYPE_ARRAY : PARAMS_TYPE_DOUCMENT);
    }
    fs_stream_write_int16(os, size());
    FSParams::params_iterate iter;
    
    for(iter = begin() ; iter != end() ; iter++){
        
        params_type type = (*iter)->type;
        
        if(type_hash){
            fs_stream_write_string(os, (*iter)->key, strlen( (*iter)->key ));
        }
        
        switch (type) {
            case PARAMS_TYPE_INT:
                fs_stream_write_byte(os, PARAMS_TYPE_INT);
                fs_stream_write_int32(os,  get<int>( (*iter)->key ) );
                break;
            case PARAMS_TYPE_ARRAY:
                get<FSParams*>( (*iter)->key )->write_to_stream(os);
                break;
            case PARAMS_TYPE_BOOL:
                fs_stream_write_byte(os, PARAMS_TYPE_BOOL);
                fs_stream_write_byte(os,  get<bool>( (*iter)->key ) ? 1 : 0 );
                break;
            case PARAMS_TYPE_DOUBLE:
                fs_stream_write_byte(os, PARAMS_TYPE_DOUBLE);
                fs_stream_write_double(os,  get<double>( (*iter)->key ) );
                break;
            case PARAMS_TYPE_DOUCMENT:
                fs_stream_write_byte(os, PARAMS_TYPE_DOUCMENT);
                get<FSParams*>( (*iter)->key )->write_to_stream(os);
                break;
            case PARAMS_TYPE_INT64:
                fs_stream_write_byte(os, PARAMS_TYPE_INT64);
                fs_stream_write_int64(os,  get<long long>( (*iter)->key ) );
                break;
            case PARAMS_TYPE_UTF8:
                fs_stream_write_byte(os, PARAMS_TYPE_UTF8);
                fs_stream_write_string(os, get<const char*>( (*iter)->key ), strlen(  get<const char*>( (*iter)->key ) ));
                break;
            case PARAMS_TYPE_NULL:
                fs_stream_write_byte(os, PARAMS_TYPE_NULL);
                break;
            default:
                break;
        }
        
    }
    
}

void FSParams::init_from_stream(struct fs_input_stream* is, fs_bool check_head){
    
    fs_bool inval = true;
    if(check_head){
        params_type type = (params_type)fs_stream_read_byte(is);
        inval = (type == PARAMS_TYPE_DOUCMENT || type == PARAMS_TYPE_ARRAY);
    }
    if(inval){
        uint16_t size = fs_stream_read_uint16(is);
        
        for(int i = 0 ; i < size ; i++){
            char* key = NULL;
            
            if(type_hash){
                fs_stream_read_string(is, &key);
            }else{
                key = (char*)fs_malloc(12);
                sprintf(key, "%d", i);
            }
            
            params_type child_type = (params_type)fs_stream_read_byte(is);
            
            switch (child_type) {
                case PARAMS_TYPE_INT:
                    set(key, fs_stream_read_int32(is));
                    break;
                case PARAMS_TYPE_INT64:
                    set(key, fs_stream_read_int64(is));
                    break;
                case PARAMS_TYPE_BOOL:
                    set(key, fs_stream_read_byte(is) == 1);
                    break;
                case PARAMS_TYPE_DOUBLE:
                    set(key, fs_stream_read_double(is));
                    break;
                case PARAMS_TYPE_ARRAY:
                {
                    FSParamsArray array;
                    array.init_from_stream(is, false);
                    set(key, array, PARAMS_TYPE_ARRAY);
                    break;
                }
                case PARAMS_TYPE_DOUCMENT:{
                    FSParamsHash hash;
                    hash.init_from_stream(is, false);
                    set(key, hash, PARAMS_TYPE_DOUCMENT);
                    break;
                }
                case PARAMS_TYPE_UTF8:{
                    char* value;
                    size_t len = fs_stream_read_string(is, &value);
                    set(key, (void*)value, (uint32_t)len);
                    break;
                }
                case PARAMS_TYPE_NULL:
                    set(key, NULL, 0, PARAMS_TYPE_NULL);
                    break;
                default:
                    break;
            }
            if(key){
                fs_free(key);
            }
        }
    }
    
}

void FSParams::debug_print(int deep) const{
    
    
    std::string tab = "";
    
    for(int i = 0 ;i < deep ; i++){
        tab += "\t";
    }
    
    params_iterate iter = this->begin();
    
    printf("%s=== FSParams debug_print begin == \n", tab.c_str());
    printf("%s|  count = %d \n",  tab.c_str(), (int)(this->end() - this->begin()));
    
    for( ; iter != this->end() ; iter ++){
        
        printf("%s|  key = %s\t",  tab.c_str(), (*iter)->getKey());
        printf("val_type = %d\t",  (*iter)->getType());
        
        switch ((*iter)->getType()) {
            case PARAMS_TYPE_INT:
                printf("val = %d\n", get<int>((*iter)->getKey()));
                break;
            case PARAMS_TYPE_INT64:
                printf("val = %lld\n", get<long long>((*iter)->getKey()));
                break;
            case PARAMS_TYPE_BOOL:
                printf("val = %s\n", get<bool>((*iter)->getKey()) ? "true" : "false");
                break;
            case PARAMS_TYPE_NULL:
                printf("val = NULL\n");
                break;
            case PARAMS_TYPE_DOUBLE:
                printf("val = %f\n", get<double>((*iter)->getKey()));
                break;
            case PARAMS_TYPE_UTF8:
                printf("val = %s\n", get<const char*>((*iter)->getKey()));
                break;
            case PARAMS_TYPE_DOUCMENT:
                printf("val = child_doucment \n");
                printf("%s------------------------------\n", tab.c_str());
                this->get<FSParams*>((*iter)->getKey())->debug_print(deep + 1);
                printf("%s------------------------------\n", tab.c_str());
                break;
            case PARAMS_TYPE_ARRAY:
                printf("val = child_array \n");
                printf("%s------------------------------\n", tab.c_str());
                this->get<FSParams*>((*iter)->getKey())->debug_print(deep + 1);
                printf("%s------------------------------\n", tab.c_str());
            default:
                break;
        }
        
        
    }
    
    printf("%s=== FSParams debug_print begin == \n", tab.c_str());
    
}
