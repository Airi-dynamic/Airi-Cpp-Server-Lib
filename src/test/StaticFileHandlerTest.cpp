#include <gtest/gtest.h>

#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "http/StaticFileHandler.h"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

// 辅助：创建临时测试文件
// 注意：每个测试实例必须使用独一无二的临时文件路径，否则 ctest 并行运行时
// 一个 fixture 的 TearDown() 会在另一个 fixture 测试中间删除文件。
class StaticFileHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // 用 pid + 进程内递增 id 生成唯一路径，避免同名类别名下多个 TEST_F 实例 / 并行进程冲突
        static std::atomic<int> kCounter{0};
        char buf[128];
        std::snprintf(buf, sizeof(buf), "/tmp/mcpp_test_static_file_%d_%d.txt",
                      static_cast<int>(::getpid()), kCounter.fetch_add(1));
        tmpPath_ = buf;
        std::ofstream ofs(tmpPath_, std::ios::binary);
        ofs << "Hello, World! This is test content for StaticFileHandler.";
        ofs.close();
    }

    void TearDown() override { std::remove(tmpPath_.c_str()); }

    HttpRequest makeGet(const std::string &url = "/") {
        HttpRequest req;
        req.setMethod("GET");
        req.setUrl(url);
        return req;
    }

    std::string tmpPath_;
};

// ── 基本文件服务 ──────────────────────────────────────────────

TEST_F(StaticFileHandlerTest, ServeExistingFile) {
    HttpRequest req = makeGet("/test.txt");
    HttpResponse resp;

    bool found = StaticFileHandler::serve(req, &resp, tmpPath_);

    EXPECT_TRUE(found);
    EXPECT_EQ(resp.statusCode(), HttpResponse::StatusCode::k200OK);
    EXPECT_TRUE(resp.hasSendFile());
    EXPECT_EQ(resp.sendFilePath(), tmpPath_);
    EXPECT_FALSE(resp.header("ETag").empty());
    EXPECT_FALSE(resp.header("Last-Modified").empty());
    EXPECT_EQ(resp.header("Accept-Ranges"), "bytes");
}

// ── 文件不存在 ────────────────────────────────────────────────

TEST_F(StaticFileHandlerTest, FileNotFound) {
    HttpRequest req = makeGet("/nope.txt");
    HttpResponse resp;

    bool found = StaticFileHandler::serve(req, &resp, "/tmp/mcpp_nonexistent_12345.txt");

    EXPECT_FALSE(found);
}

// ── ETag 304 Not Modified ─────────────────────────────────────

TEST_F(StaticFileHandlerTest, ETagConditionalGet304) {
    // 第一次请求获取 ETag
    HttpRequest req1 = makeGet("/test.txt");
    HttpResponse resp1;
    StaticFileHandler::serve(req1, &resp1, tmpPath_);
    std::string etag = resp1.header("ETag");
    ASSERT_FALSE(etag.empty());

    // 第二次请求带 If-None-Match
    HttpRequest req2 = makeGet("/test.txt");
    req2.addHeader("If-None-Match", etag);
    HttpResponse resp2;
    StaticFileHandler::serve(req2, &resp2, tmpPath_);

    EXPECT_EQ(resp2.statusCode(), HttpResponse::StatusCode::k304NotModified);
    EXPECT_TRUE(resp2.body().empty());
    EXPECT_FALSE(resp2.hasSendFile());
}

// ── Range 206 Partial Content ─────────────────────────────────

TEST_F(StaticFileHandlerTest, RangePartialContent) {
    HttpRequest req = makeGet("/test.txt");
    req.addHeader("Range", "bytes=0-4");
    HttpResponse resp;

    bool found = StaticFileHandler::serve(req, &resp, tmpPath_);

    EXPECT_TRUE(found);
    EXPECT_EQ(resp.statusCode(), HttpResponse::StatusCode::k206PartialContent);
    EXPECT_TRUE(resp.hasSendFile());
    EXPECT_EQ(resp.sendFileOffset(), 0u);
    EXPECT_EQ(resp.sendFileCount(), 5u);
    EXPECT_FALSE(resp.header("Content-Range").empty());
}

// ── Range 后缀 bytes=-5 ──────────────────────────────────────

TEST_F(StaticFileHandlerTest, RangeSuffix) {
    HttpRequest req = makeGet("/test.txt");
    req.addHeader("Range", "bytes=-5");
    HttpResponse resp;

    bool found = StaticFileHandler::serve(req, &resp, tmpPath_);

    EXPECT_TRUE(found);
    EXPECT_EQ(resp.statusCode(), HttpResponse::StatusCode::k206PartialContent);
    EXPECT_EQ(resp.sendFileCount(), 5u);
}

// ── Range 不合法 → 416 ───────────────────────────────────────

TEST_F(StaticFileHandlerTest, InvalidRange416) {
    HttpRequest req = makeGet("/test.txt");
    req.addHeader("Range", "bytes=99999-");
    HttpResponse resp;

    bool found = StaticFileHandler::serve(req, &resp, tmpPath_);

    EXPECT_TRUE(found);
    EXPECT_EQ(resp.statusCode(), HttpResponse::StatusCode::k416RangeNotSatisfiable);
}

// ── 路径遍历拦截 ──────────────────────────────────────────────

TEST_F(StaticFileHandlerTest, PathTraversalBlocked) {
    HttpRequest req = makeGet("/../etc/passwd");
    HttpResponse resp;

    bool found = StaticFileHandler::serve(req, &resp, "/tmp/../etc/passwd");

    EXPECT_TRUE(found); // 返回 true 表示已处理（400），非文件不存在
    EXPECT_EQ(resp.statusCode(), HttpResponse::StatusCode::k400BadRequest);
}

// ── 下载名 Content-Disposition ────────────────────────────────

TEST_F(StaticFileHandlerTest, DownloadName) {
    HttpRequest req = makeGet("/test.txt");
    HttpResponse resp;

    StaticFileHandler::Options opts;
    opts.downloadName = "report.txt";
    StaticFileHandler::serve(req, &resp, tmpPath_, opts);

    std::string cd = resp.header("Content-Disposition");
    EXPECT_NE(cd.find("attachment"), std::string::npos);
    EXPECT_NE(cd.find("report.txt"), std::string::npos);
}
