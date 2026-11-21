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

// Writes the projects Tests/merge/git_driver_test.sh commits: for each case, the common ancestor
// and the two sides' edits of it, as <dir>/<case>-base.ntpz, -mine.ntpz and -theirs.ntpz.

#include "Global/Macros.h"

#include <exception>
#include <functional>
#include <iostream>
#include <string>

#include "MergeScenarios.h"
#include "Serialization/Bundle/ProjectBundle.h"

NATRON_NAMESPACE_USING
using namespace MergeScenarios;

namespace {

void
write(const std::string& path,
      const std::function<void(SimGraph*)>& edit)
{
    SimGraph graph = templateGraph("b:");
    if (edit) {
        edit(&graph);
    }
    Serialization::ProjectBundleWriter writer;
    writer.setProject(graph.project());
    writer.setRootFolder("shot");
    writer.write(path);
}

} // namespace

int
main(int argc,
     char* argv[])
{
    if (argc != 2) {
        std::cerr << "usage: merge_scenario_writer <dir>\n";

        return 2;
    }
    const std::string dir = argv[1];
    try {
        write(dir + "/clean-base.ntpz", nullptr);
        write(dir + "/clean-mine.ntpz", [](SimGraph* g) {
            g->rename("Grade1", "KeyGrade");
            g->set("KeyGrade", "multiply", { 1.5 });
        });
        write(dir + "/clean-theirs.ntpz", [](SimGraph* g) { g->set("Blur1", "size", { 4., 4. }); });
        write(dir + "/conflict-base.ntpz", nullptr);
        write(dir + "/conflict-mine.ntpz", [](SimGraph* g) { g->set("Blur1", "size", { 3., 3. }); });
        write(dir + "/conflict-theirs.ntpz", [](SimGraph* g) { g->set("Blur1", "size", { 4., 4. }); });
        write(dir + "/conflict-more.ntpz", [](SimGraph* g) {
            g->set("Blur1", "size", { 4., 4. });
            g->set("Grade1", "multiply", { 2. });
        });
        write(dir + "/addadd-mine.ntpz", [](SimGraph* g) { g->add("MineBlur", "Blur", "m:MineBlur", { { "Source", "ColorCorrect1" } }); });
        write(dir + "/addadd-theirs.ntpz", [](SimGraph* g) { g->add("TheirBlur", "Blur", "t:TheirBlur", { { "Source", "ColorCorrect1" } }); });
    } catch (const std::exception& e) {
        std::cerr << "merge_scenario_writer: " << e.what() << "\n";

        return 2;
    }

    return 0;
}
