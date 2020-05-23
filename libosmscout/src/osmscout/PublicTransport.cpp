/*
  This source is part of the libosmscout library
  Copyright (C) 2020  Tim Teulings

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
*/

#include <osmscout/PublicTransport.h>

namespace osmscout {

  void PTRoute::Variant::SetName(const std::string& name)
  {
    PTRoute::Variant::name=name;
  }

  void PTRoute::Variant::SetRef(const std::string& ref)
  {
    PTRoute::Variant::ref=ref;
  }

  void PTRoute::Variant::SetOperator(const std::string& operatorName)
  {
    PTRoute::Variant::operatorName=operatorName;
  }

  void PTRoute::Variant::SetNetwork(const std::string& network)
  {
    PTRoute::Variant::network=network;
  }

  void PTRoute::SetType(const TypeInfoRef& type)
  {
    PTRoute::type=type;
  }

  void PTRoute::SetName(const std::string& name)
  {
    PTRoute::name=name;
  }

  void PTRoute::SetRef(const std::string& ref)
  {
    PTRoute::ref=ref;
  }

  void PTRoute::SetOperator(const std::string& operatorName)
  {
    PTRoute::operatorName=operatorName;
  }

  void PTRoute::SetNetwork(const std::string& network)
  {
    PTRoute::network=network;
  }

  void PTRoute::Read(const TypeConfig& typeConfig,
                     FileScanner& scanner)
  {
    fileOffset=scanner.GetPos();

    uint16_t typeIndex;

    scanner.Read(typeIndex);

    type=typeConfig.GetTypeInfo(typeIndex);

    scanner.Read(name);
    scanner.Read(ref);
    scanner.Read(operatorName);
    scanner.Read(network);

    uint32_t variantCount;

    scanner.ReadNumber(variantCount);

    variants.resize(variantCount);

    for (auto& variant : variants) {
      scanner.Read(variant.name);
      scanner.Read(variant.ref);
      scanner.Read(variant.operatorName);
      scanner.Read(variant.network);
    }
  }

  void PTRoute::Write(const TypeConfig& /*typeConfig*/,
                      FileWriter& writer) const
  {
    // TODO: Find a better way to get and store a stable id for the type
    writer.Write(static_cast<uint16_t>(type->GetIndex()));
    writer.Write(name);
    writer.Write(ref);
    writer.Write(operatorName);
    writer.Write(network);

    writer.WriteNumber(variants.size());

    for (const auto& variant : variants) {
      writer.Write(variant.name);
      writer.Write(variant.ref);
      writer.Write(variant.operatorName);
      writer.Write(variant.network);
    }
  }
}
