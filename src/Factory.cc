/*
 * Copyright (C) 2016 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4251)
#endif

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/text_format.h>

#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <string>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "ignition/msgs/Factory.hh"
#include "ignition/msgs/Filesystem.hh"

using namespace ignition;
using namespace msgs;

/// \def ProtobufUniquePtr
/// \brief An abbreviated unique pointer to a protobuf message type.
using ProtoUniquePtr = std::unique_ptr<google::protobuf::Message>;

std::map<std::string, FactoryFn> *Factory::msgMap = NULL;

/// \brief Initialize the descriptor pool.
static google::protobuf::DescriptorPool pool;

//////////////////////////////////////////////////
/// \brief split at a one character delimiter to get a vector of something
/// \param[in] _orig The string to split
/// \param[in] _delim a character to split the string at
/// \returns vector of split pieces of the string excluding the delimiter
std::vector<std::string> split(const std::string &_orig, char _delim)
{
  std::vector<std::string> pieces;
  size_t pos1 = 0;
  size_t pos2 = _orig.find(_delim);
  while (pos2 != std::string::npos)
  {
    pieces.push_back(_orig.substr(pos1, pos2-pos1));
    pos1 = pos2+1;
    pos2 = _orig.find(_delim, pos2+1);
  }
  pieces.push_back(_orig.substr(pos1, _orig.size()-pos1));
  return pieces;
}

/////////////////////////////////////////////////
/// \brief A factory class to generate protobuf messages at runtime based on
/// their message descriptions. The location of the .desc files is expected
/// via the IGN_DESCRIPTOR_DIR environment variable. This environment variable
/// expects paths to directories containing .desc files. Any file without the
/// .desc extension will be ignored.
class DynamicFactory
{
  /////////////////////////////////////////////////
  /// \brief Constructor.
  public: DynamicFactory()
  {
    const char *ignDescPath = std::getenv("IGN_DESCRIPTOR_DIR");
    if (!ignDescPath)
      return;

    // Split all the directories containing .desc files.
    auto descDirs = split(ignDescPath, ':');

    for (const auto descDir : descDirs)
    {
      for (DirIter dirIter(descDir); dirIter != DirIter(); ++dirIter)
      {
        // Ignore files without the .desc extension.
        if ((*dirIter).rfind(".desc") == std::string::npos)
          continue;

        // Parse the .desc file.
        std::ifstream ifs(*dirIter);
        if (!ifs.is_open())
        {
          std::cerr << "DynamicFactory(): Unable to open [" << *dirIter << "]"
                    << std::endl;
          continue;
        }

        google::protobuf::FileDescriptorSet fileDescriptorSet;
        if (!fileDescriptorSet.ParseFromIstream(&ifs))
        {
          std::cerr << "DynamicFactory(): Unable to parse descriptor set from ["
                    << *dirIter << "]" << std::endl;
          continue;
        }

        // Place the real descriptors in the descriptor pool.
        for (const auto &fileDescriptorProto : fileDescriptorSet.file())
        {
          if (!pool.BuildFile(fileDescriptorProto))
          {
            std::cerr << "DynamicFactory(). Unable to place descriptors from ["
                      << *dirIter << "] in the descriptor pool" << std::endl;
          }
        }
      }
    }
  }

  /////////////////////////////////////////////////
  /// \brief Create a new instance of a message.
  /// \param[in] _msgType Type of message to create.
  /// \return Pointer to a google protobuf message. Null if the message
  /// type could not be handled.
  public: static ProtoUniquePtr New(const std::string &_msgType)
  {
    // Shortcut if the type has been already registered.
    auto msgF = msgMap.find(_msgType);
    if (msgF != msgMap.end())
      return msgF->second();

    static google::protobuf::DynamicMessageFactory dynamicMessageFactory;

    // Nothing to do if we don't know about this type in the descriptor map.
    auto descriptor = pool.FindMessageTypeByName(_msgType);
    if (!descriptor)
      return nullptr;

    auto msgPtr(dynamicMessageFactory.GetPrototype(descriptor)->New());

    // Create the lambda for registration purposes.
    std::function<ProtoUniquePtr()> f = [msgPtr]() -> ProtoUniquePtr
    {
      ProtoUniquePtr ptr(msgPtr->New());
      return ptr;
    };

    // Register the new type for the future.
    msgMap[_msgType] = f;

    return f();
  }

  /// \brief A list of registered message types built at runtime.
  /// The key is the message type. The value is a function that returns a
  /// std::unique_ptr to a new empty instance of the message or nullptr if the
  /// message is not registered.
  public: static std::map<std::string, std::function<ProtoUniquePtr()>> msgMap;
};

/// \brief Initialize the Protobuf factory for runtime messages.
static DynamicFactory dynamicFactory;

/// \brief Initialize the map inside the DynamicFactory class.
std::map<std::string, std::function<ProtoUniquePtr()>> DynamicFactory::msgMap;

/////////////////////////////////////////////////
void Factory::Register(const std::string &_msgType,
                       FactoryFn _factoryfn)
{
  // Create the msgMap if it's null
  if (!msgMap)
    msgMap = new std::map<std::string, FactoryFn>;

  (*msgMap)[_msgType] = _factoryfn;
}

/////////////////////////////////////////////////
std::unique_ptr<google::protobuf::Message> Factory::New(
    const std::string &_msgType)
{
  std::unique_ptr<google::protobuf::Message> msg;

  std::string type;
  // Convert "ignition.msgs." to "ign_msgs.".
  if (_msgType.find("ignition.msgs.") == 0)
  {
    type = "ign_msgs." + _msgType.substr(14);
  }
  // Convert ".ignition.msgs." to "ign_msgs.".
  else if (_msgType.find(".ignition.msgs.") == 0)
  {
    type = "ign_msgs." + _msgType.substr(15);
  }
  else
  {
    // Fix typenames that are missing "ign_msgs." at the beginning.
    if (_msgType.find("ign_msgs.") != 0)
      type = "ign_msgs.";
    type += _msgType;
  }

  // Create a new message if a FactoryFn has been assigned to the message type
  if (msgMap->find(type) != msgMap->end())
    return ((*msgMap)[type]) ();

  // Check if we have the message descriptor.
  msg = dynamicFactory.New(_msgType);

  return msg;
}

/////////////////////////////////////////////////
std::unique_ptr<google::protobuf::Message> Factory::New(
    const std::string &_msgType, const std::string &_args)
{
  auto msg = New(_msgType);
  if (msg)
  {
    google::protobuf::TextFormat::ParseFromString(_args, msg.get());
  }

  return msg;
}

/////////////////////////////////////////////////
void Factory::Types(std::vector<std::string> &_types)
{
  _types.clear();

  // Return the list of all known message types.
  std::map<std::string, FactoryFn>::const_iterator iter;
  for (iter = msgMap->begin(); iter != msgMap->end(); ++iter)
  {
    _types.push_back(iter->first);
  }
}
