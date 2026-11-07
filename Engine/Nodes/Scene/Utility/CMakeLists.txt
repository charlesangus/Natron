# The sources here compile into NatronEngine through the recursive globs in Engine/CMakeLists.txt.
# This file carries what only they need.
target_link_libraries(NatronEngine PRIVATE embree)
