/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#include "ProjectGuiXml.h"

// explicit template instantiations

namespace boost {
namespace serialization {

    template void serialize<boost::archive::xml_oarchive>(boost::archive::xml_oarchive& ar,
                                                          NATRON_NAMESPACE::Serialization::NodeGuiSerialization& node,
                                                          const unsigned int version);

    template void serialize<boost::archive::xml_oarchive>(boost::archive::xml_oarchive& ar,
                                                          NATRON_NAMESPACE::Serialization::ViewerDataSerialization& viewer,
                                                          const unsigned int version);

    template void serialize<boost::archive::xml_oarchive>(boost::archive::xml_oarchive& ar,
                                                          NATRON_NAMESPACE::Serialization::PythonPanelSerialization& panel,
                                                          const unsigned int version);

    template void serialize<boost::archive::xml_oarchive>(boost::archive::xml_oarchive& ar,
                                                          NATRON_NAMESPACE::Serialization::PaneLayoutSerialization& pane,
                                                          const unsigned int version);

    template void serialize<boost::archive::xml_oarchive>(boost::archive::xml_oarchive& ar,
                                                          NATRON_NAMESPACE::Serialization::SplitterSerialization& splitter,
                                                          const unsigned int version);

    template void serialize<boost::archive::xml_oarchive>(boost::archive::xml_oarchive& ar,
                                                          NATRON_NAMESPACE::Serialization::ApplicationWindowSerialization& window,
                                                          const unsigned int version);

    template void serialize<boost::archive::xml_oarchive>(boost::archive::xml_oarchive& ar,
                                                          NATRON_NAMESPACE::Serialization::GuiLayoutSerialization& layout,
                                                          const unsigned int version);

    template void serialize<boost::archive::xml_oarchive>(boost::archive::xml_oarchive& ar,
                                                          NATRON_NAMESPACE::Serialization::ProjectGuiSerialization& projectGui,
                                                          const unsigned int version);

} // namespace serialization
} // namespace boost
