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

#include <gtest/gtest.h>

#include <vector>
#include <cstring>

#include <boost/format.hpp>

#include "libscriptobject_gason.h"
#include "script_object_factory.h"
#include "script_array_factory.h"
#include "script_array_vector_source.h"

#include "libhttpserver.h"

#include "../databases.h"
#include "../database.h"
#include "../rest_exceptions.h"
#include "../post_all_documents_options.h"

class BasicDatabaseTests : public ::testing::Test {
protected:
    BasicDatabaseTests() {

    }
    
    static void SetUpTestCase() {
        auto dbName = "BasicDatabaseTests";
        databases_.AddDatabase(dbName);
        db_ = databases_.GetDatabase(dbName);
        
        std::string json = R"({"docs":[)";
        for (auto i = 0; i < 1000; ++i) {
            if (i > 0) {
                json += ',';
            }
            
            auto id = MakeDocId(i);
            json += MakeDocJson(id);
        }
        json += R"(]}\0)";
        
        std::vector<char> buffer{json.cbegin(), json.cend()};
        rs::scriptobject::ScriptObjectJsonSource source(buffer.data());        
        auto obj = rs::scriptobject::ScriptObjectFactory::CreateObject(source, false);
        docs_ = obj->getArray("docs");
    }
    
    virtual void SetUp() {
        
    }
    
    virtual void TearDown() {
        
    }
    
    static std::string MakeDocId(unsigned id) {
        return (boost::format("%08u") % id).str();
    }
    
    static std::string MakeDocJson(const std::string& id) {
        return MakeDocJson(id.c_str());
    }
    
    static std::string MakeDocJson(const char* id) {
        auto json = (boost::format(R"({"_id":"%s","num":42,"sunny":true,"pi":3.14159,"lorem":"ipsum","obj":{},"arr":[]})") % id).str();
        return json;
    }
    
    static bool ValidateRevision(unsigned num, document_ptr doc) {
        return ValidateRevision(num, doc->getRev());
    }
    
    static bool ValidateRevision(unsigned num, const std::string& rev) {
        return ValidateRevision(num, rev.c_str());
    }
    
    static bool ValidateRevision(unsigned num, const char* rev) {
        auto valid = false;
        
        if (rev) {
            auto len = std::strlen(rev);
            if (len == 34) {
                valid = rev[0] == ('0' + num) && rev[1] == '-';
            }
        }
        
        return valid;
    }
    
    static Databases databases_;
    static database_ptr db_;
    static script_array_ptr docs_;
    static unsigned updateSequence_;
};

Databases BasicDatabaseTests::databases_;
database_ptr BasicDatabaseTests::db_;
script_array_ptr BasicDatabaseTests::docs_;
unsigned BasicDatabaseTests::updateSequence_ = 0;

TEST_F(BasicDatabaseTests, test0) {
    ASSERT_EQ(0, db_->DocCount());
    ASSERT_EQ(0, db_->DocDelCount());
    ASSERT_EQ(0, db_->DiskSize());    
    ASSERT_EQ(0, db_->DataSize());
    ASSERT_EQ(0, db_->UpdateSequence());
    ASSERT_EQ(0, db_->CommitedUpdateSequence());
    ASSERT_EQ(0, db_->PurgeSequence());
}

TEST_F(BasicDatabaseTests, test1) {    
    auto obj = docs_->getObject(0);
    auto id = obj->getString("_id");
    
    auto doc = db_->SetDocument(id, obj);
            
    ASSERT_EQ(1, db_->DocCount());
    ASSERT_EQ(++updateSequence_, db_->UpdateSequence());    
    ASSERT_NE(nullptr, doc);
    ASSERT_STREQ(id, doc->getId());
    ASSERT_TRUE(ValidateRevision(1, doc));
    
    auto delDoc = db_->DeleteDocument(id, doc->getRev());
    ASSERT_NE(nullptr, delDoc);
    ASSERT_STREQ(doc->getRev(), delDoc->getRev());
    
    ASSERT_EQ(0, db_->DocCount());
    ASSERT_EQ(++updateSequence_, db_->UpdateSequence());
}

TEST_F(BasicDatabaseTests, test2) {    
    auto obj = docs_->getObject(0);
    auto id = obj->getString("_id");
    
    auto doc = db_->SetDocument(id, obj);
            
    ASSERT_EQ(1, db_->DocCount());
    ASSERT_EQ(++updateSequence_, db_->UpdateSequence());
    ASSERT_NE(nullptr, doc);
    ASSERT_STREQ(id, doc->getId());
    ASSERT_TRUE(ValidateRevision(1, doc));
    
    auto delDoc = db_->DeleteDocument(id, doc->getRev());
    ASSERT_NE(nullptr, delDoc);
    ASSERT_STREQ(doc->getRev(), delDoc->getRev());
    
    ASSERT_EQ(0, db_->DocCount());
    ASSERT_EQ(++updateSequence_, db_->UpdateSequence());
}

TEST_F(BasicDatabaseTests, test3) {    
    auto obj1 = docs_->getObject(0);
    auto id1 = obj1->getString("_id");    
    auto doc1 = db_->SetDocument(id1, obj1);
            
    ASSERT_EQ(1, db_->DocCount());
    ASSERT_EQ(++updateSequence_, db_->UpdateSequence());
    ASSERT_NE(nullptr, doc1);
    ASSERT_STREQ(id1, doc1->getId());
    ASSERT_TRUE(ValidateRevision(1, doc1));
    
    auto obj2 = docs_->getObject(1);
    auto id2 = obj2->getString("_id");    
    auto doc2 = db_->SetDocument(id2, obj1);
    
    ASSERT_EQ(2, db_->DocCount());
    ASSERT_EQ(++updateSequence_, db_->UpdateSequence());
    ASSERT_NE(nullptr, doc2);
    ASSERT_STREQ(id2, doc2->getId());
    ASSERT_TRUE(ValidateRevision(1, doc2));
    
    auto delDoc1 = db_->DeleteDocument(id1, doc1->getRev());
    ASSERT_NE(nullptr, delDoc1);
    ASSERT_STREQ(doc1->getRev(), delDoc1->getRev());
    
    ASSERT_EQ(1, db_->DocCount());
    ASSERT_EQ(++updateSequence_, db_->UpdateSequence());
    
    auto delDoc2 = db_->DeleteDocument(id2, doc2->getRev());
    ASSERT_NE(nullptr, delDoc2);
    ASSERT_STREQ(doc2->getRev(), delDoc2->getRev());
    
    ASSERT_EQ(0, db_->DocCount());
    ASSERT_EQ(++updateSequence_, db_->UpdateSequence());
}

TEST_F(BasicDatabaseTests, test4) {    
    auto obj1 = docs_->getObject(0);
    auto id1 = obj1->getString("_id");    
    auto doc1 = db_->SetDocument(id1, obj1);
            
    ASSERT_EQ(1, db_->DocCount());
    ASSERT_EQ(++updateSequence_, db_->UpdateSequence());
    ASSERT_NE(nullptr, doc1);
    ASSERT_STREQ(id1, doc1->getId());
    ASSERT_TRUE(ValidateRevision(1, doc1));
    
    auto doc2 = db_->SetDocument(id1, doc1->getObject());
    ASSERT_EQ(1, db_->DocCount());
    ASSERT_EQ(++updateSequence_, db_->UpdateSequence());
    ASSERT_NE(nullptr, doc2);
    ASSERT_STREQ(id1, doc2->getId());
    ASSERT_TRUE(ValidateRevision(2, doc2));
    
    auto delDoc2 = db_->DeleteDocument(id1, doc2->getRev());
    ASSERT_NE(nullptr, delDoc2);
    ASSERT_STREQ(doc2->getRev(), delDoc2->getRev());
    
    ASSERT_EQ(0, db_->DocCount());
    ASSERT_EQ(++updateSequence_, db_->UpdateSequence());
}

TEST_F(BasicDatabaseTests, test5) {
    auto obj1 = docs_->getObject(0);
    auto id1 = obj1->getString("_id");
    auto doc1 = db_->SetDocument(id1, obj1);

    ASSERT_EQ(1, db_->DocCount());
    ASSERT_EQ(++updateSequence_, db_->UpdateSequence());
    ASSERT_NE(nullptr, doc1);
    ASSERT_STREQ(id1, doc1->getId());
    ASSERT_TRUE(ValidateRevision(1, doc1));
    
    auto doc2 = db_->GetDocument(id1);
    ASSERT_EQ(1, db_->DocCount());
    ASSERT_EQ(updateSequence_, db_->UpdateSequence());
    ASSERT_NE(nullptr, doc2);
    ASSERT_STREQ(id1, doc2->getId());
    ASSERT_TRUE(ValidateRevision(1, doc2));
    
    auto delDoc2 = db_->DeleteDocument(id1, doc2->getRev());
    ASSERT_NE(nullptr, delDoc2);
    ASSERT_STREQ(doc2->getRev(), delDoc2->getRev());
    
    ASSERT_EQ(0, db_->DocCount());
    ASSERT_EQ(++updateSequence_, db_->UpdateSequence());
}

TEST_F(BasicDatabaseTests, test6) {
    auto id1 = MakeDocId(0);
    auto doc1 = db_->GetDocument(id1.c_str(), false);

    ASSERT_EQ(0, db_->DocCount());
    ASSERT_EQ(updateSequence_, db_->UpdateSequence());
    ASSERT_EQ(nullptr, doc1);
    
    ASSERT_THROW({
        db_->GetDocument(id1.c_str(), true);
    }, DocumentMissing);
}

TEST_F(BasicDatabaseTests, test7) {    
    auto results = db_->PostBulkDocuments(docs_, true);
    ASSERT_EQ(docs_->getCount(), results.size());
    
    ASSERT_EQ(docs_->getCount(), db_->DocCount());
    ASSERT_EQ(docs_->getCount() + updateSequence_, db_->UpdateSequence());
    
    for (auto i = 0; i < results.size(); ++i) {
        auto result = results[i];
        ASSERT_STREQ(docs_->getObject(i)->getString("_id"), result.id_.c_str());
        ASSERT_TRUE(ValidateRevision(1, result.rev_));
    }       
    
    for (auto i = 0; i < results.size(); ++i) {
        auto result = results[i];
        
        db_->DeleteDocument(result.id_.c_str(), result.rev_.c_str());
        ASSERT_EQ(docs_->getCount() - i - 1, db_->DocCount());
    }
    
    ASSERT_EQ(0, db_->DocCount());
    ASSERT_EQ((docs_->getCount() * 2) + updateSequence_, db_->UpdateSequence());
    updateSequence_ += docs_->getCount() * 2;
}

TEST_F(BasicDatabaseTests, test8) {
    auto results = db_->PostBulkDocuments(docs_, true);
    ASSERT_EQ(docs_->getCount(), results.size());
    
    ASSERT_EQ(docs_->getCount(), db_->DocCount());
    ASSERT_EQ(docs_->getCount() + updateSequence_, db_->UpdateSequence());
    
    for (auto i = 0; i < results.size(); ++i) {
        auto result = results[i];
        auto doc = db_->GetDocument(result.id_.c_str(), false);
        ASSERT_NE(nullptr, doc);
        ASSERT_STREQ(result.id_.c_str(), doc->getId());
        ASSERT_STREQ(result.rev_.c_str(), doc->getRev());
        ASSERT_TRUE(ValidateRevision(1, doc));
    }
    
    for (long i = results.size() - 1; i >= 0; --i) {
        auto result = results[i];
        auto doc = db_->GetDocument(result.id_.c_str(), false);
        ASSERT_NE(nullptr, doc);
        ASSERT_STREQ(result.id_.c_str(), doc->getId());
        ASSERT_STREQ(result.rev_.c_str(), doc->getRev());
        ASSERT_TRUE(ValidateRevision(1, doc));
    }
    
    updateSequence_ += docs_->getCount();
}

TEST_F(BasicDatabaseTests, test9) {
    rs::scriptobject::utils::ArrayVector ids;
    for (long i = docs_->getCount() - 1; i >= 0; --i) {
        auto id = MakeDocId(i);
        ids.emplace_back(id.c_str());
    }
    
    rs::scriptobject::utils::ScriptArrayVectorSource idSource{ids};    
    auto keys = rs::scriptobject::ScriptArrayFactory::CreateArray(idSource);
    
    rs::httpserver::QueryString qs{""};    
    PostAllDocumentsOptions options{qs, keys};    
    DocumentsCollection::size_type totalDocs = 0, updateSequence = 0;    
    auto results = db_->PostDocuments(options, totalDocs, updateSequence);
    
    ASSERT_EQ(docs_->getCount(), results->size());
    for (auto i = 0; i < results->size(); ++i) {
        auto result = (*results)[i];
        auto doc = docs_->getObject(docs_->getCount() - 1 - i);
        ASSERT_STREQ(doc->getString("_id"), result->getId());
    }
}

TEST_F(BasicDatabaseTests, test10) {
    rs::scriptobject::utils::ArrayVector ids;
    for (auto i = 0; i < docs_->getCount(); i++) {
        auto id = MakeDocId(i);
        ids.emplace_back(id.c_str());
    }
    
    rs::scriptobject::utils::ScriptArrayVectorSource idSource{ids};    
    auto keys = rs::scriptobject::ScriptArrayFactory::CreateArray(idSource);
    
    rs::httpserver::QueryString qs{"limit=10"};
    PostAllDocumentsOptions options{qs, keys};    
    DocumentsCollection::size_type totalDocs = 0, updateSequence = 0;    
    auto results = db_->PostDocuments(options, totalDocs, updateSequence);
    
    ASSERT_EQ(10, results->size());
    for (auto i = 0; i < 10; ++i) {
        auto result = (*results)[i];
        auto doc = docs_->getObject(i);
        ASSERT_STREQ(doc->getString("_id"), result->getId());
    }
}

TEST_F(BasicDatabaseTests, test11) {
    rs::scriptobject::utils::ArrayVector ids;
    for (auto i = 0; i < docs_->getCount(); i++) {
        auto id = MakeDocId(i);
        ids.emplace_back(id.c_str());
    }
    
    rs::scriptobject::utils::ScriptArrayVectorSource idSource{ids};    
    auto keys = rs::scriptobject::ScriptArrayFactory::CreateArray(idSource);
    
    rs::httpserver::QueryString qs{"limit=10&skip=20"};
    PostAllDocumentsOptions options{qs, keys};    
    DocumentsCollection::size_type totalDocs = 0, updateSequence = 0;    
    auto results = db_->PostDocuments(options, totalDocs, updateSequence);
    
    ASSERT_EQ(10, results->size());
    for (auto i = 0; i < 10; ++i) {
        auto result = (*results)[i];
        auto doc = docs_->getObject(i + 20);
        ASSERT_STREQ(doc->getString("_id"), result->getId());
    }
}

TEST_F(BasicDatabaseTests, test12) {
    rs::scriptobject::utils::ArrayVector ids;
    for (auto i = 0; i < docs_->getCount(); i++) {
        auto id = MakeDocId(i);
        ids.emplace_back(id.c_str());
    }
    
    rs::scriptobject::utils::ScriptArrayVectorSource idSource{ids};    
    auto keys = rs::scriptobject::ScriptArrayFactory::CreateArray(idSource);
    
    rs::httpserver::QueryString qs{"descending=true"};
    PostAllDocumentsOptions options{qs, keys};    
    DocumentsCollection::size_type totalDocs = 0, updateSequence = 0;    
    auto results = db_->PostDocuments(options, totalDocs, updateSequence);
    
    ASSERT_EQ(docs_->getCount(), results->size());
    for (auto i = 0; i < results->size(); ++i) {
        auto result = (*results)[i];
        auto doc = docs_->getObject(docs_->getCount() - 1 - i);
        ASSERT_STREQ(doc->getString("_id"), result->getId());
    }
}

TEST_F(BasicDatabaseTests, test13) {
    rs::scriptobject::utils::ArrayVector ids;
    for (auto i = 0; i < docs_->getCount(); i++) {
        auto id = MakeDocId(i);
        ids.emplace_back(id.c_str());
    }
    
    rs::scriptobject::utils::ScriptArrayVectorSource idSource{ids};    
    auto keys = rs::scriptobject::ScriptArrayFactory::CreateArray(idSource);
    
    rs::httpserver::QueryString qs{"descending=true&limit=10"};
    PostAllDocumentsOptions options{qs, keys};    
    DocumentsCollection::size_type totalDocs = 0, updateSequence = 0;    
    auto results = db_->PostDocuments(options, totalDocs, updateSequence);
    
    ASSERT_EQ(10, results->size());
    for (auto i = 0; i < 10; ++i) {
        auto result = (*results)[i];
        auto doc = docs_->getObject(docs_->getCount() - 1 - i);
        ASSERT_STREQ(doc->getString("_id"), result->getId());
    }
}

TEST_F(BasicDatabaseTests, test14) {
    rs::scriptobject::utils::ArrayVector ids;
    for (auto i = 0; i < docs_->getCount(); i++) {
        auto id = MakeDocId(i);
        ids.emplace_back(id.c_str());
    }
    
    rs::scriptobject::utils::ScriptArrayVectorSource idSource{ids};    
    auto keys = rs::scriptobject::ScriptArrayFactory::CreateArray(idSource);
    
    rs::httpserver::QueryString qs{"descending=true&limit=10&skip=20"};
    PostAllDocumentsOptions options{qs, keys};    
    DocumentsCollection::size_type totalDocs = 0, updateSequence = 0;    
    auto results = db_->PostDocuments(options, totalDocs, updateSequence);
    
    ASSERT_EQ(10, results->size());
    for (auto i = 0; i < 10; ++i) {
        auto result = (*results)[i];
        auto doc = docs_->getObject(docs_->getCount() - 1 - i - 20);
        ASSERT_STREQ(doc->getString("_id"), result->getId());
    }
}