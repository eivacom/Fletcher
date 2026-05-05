# fletcher-pubsub-arrow-target.cmake
#
# Injected into every consumer's build by Conan (cmake_build_modules).
#
# Creates a convenience alias:
#   fletcher::pubsub-arrow  ->  eiva-fletcher-pubsub-arrow::eiva-fletcher-pubsub-arrow

if(TARGET eiva-fletcher-pubsub-arrow::eiva-fletcher-pubsub-arrow AND NOT TARGET fletcher::pubsub-arrow)
    add_library(fletcher::pubsub-arrow ALIAS eiva-fletcher-pubsub-arrow::eiva-fletcher-pubsub-arrow)
endif()
