#include "ipc_protocol.hpp"

#include <gtest/gtest.h>

using namespace linuxcampam::protocol;

TEST(ProtocolTest, CommandToString) {
  EXPECT_EQ(commandToString(Command::AUTH_REQUEST), "AUTH_REQUEST");
  EXPECT_EQ(commandToString(Command::ADD_USER), "ADD_USER");
  EXPECT_EQ(commandToString(Command::UNKNOWN), "UNKNOWN");
}

TEST(ProtocolTest, StringToCommand) {
  EXPECT_EQ(stringToCommand("AUTH_REQUEST"), Command::AUTH_REQUEST);
  EXPECT_EQ(stringToCommand("ADD_USER"), Command::ADD_USER);
  EXPECT_EQ(stringToCommand("INVALID_CMD"), Command::UNKNOWN);
}

TEST(ProtocolTest, SerializeRequest) {
  Request req{Command::AUTH_REQUEST, {"user1"}};
  EXPECT_EQ(req.serialize(), "AUTH_REQUEST user1");

  Request req2{Command::TRAIN_USER, {"user2", "label"}};
  EXPECT_EQ(req2.serialize(), "TRAIN_USER user2 label");

  Request req3{Command::GET_VERSION, {}};
  EXPECT_EQ(req3.serialize(), "GET_VERSION");
}

TEST(ProtocolTest, DeserializeRequest) {
  Request req = Request::deserialize("AUTH_REQUEST user1");
  EXPECT_EQ(req.cmd, Command::AUTH_REQUEST);
  ASSERT_EQ(req.args.size(), 1);
  EXPECT_EQ(req.args[0], "user1");

  Request req2 = Request::deserialize("TRAIN_USER user2 label");
  EXPECT_EQ(req2.cmd, Command::TRAIN_USER);
  ASSERT_EQ(req2.args.size(), 2);
  EXPECT_EQ(req2.args[0], "user2");
  EXPECT_EQ(req2.args[1], "label");

  Request req3 = Request::deserialize("GET_VERSION");
  EXPECT_EQ(req3.cmd, Command::GET_VERSION);
  EXPECT_TRUE(req3.args.empty());
}

TEST(ProtocolTest, DeserializeEdgeCases) {
  Request req = Request::deserialize("AUTH_REQUEST   user1  ");
  EXPECT_EQ(req.cmd, Command::AUTH_REQUEST);
  ASSERT_EQ(req.args.size(), 1);
  EXPECT_EQ(req.args[0], "user1");

  Request req2 = Request::deserialize("");
  EXPECT_EQ(req2.cmd, Command::UNKNOWN);
}
