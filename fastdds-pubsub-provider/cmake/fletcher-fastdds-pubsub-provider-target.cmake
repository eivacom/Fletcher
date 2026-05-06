# fletcher-fastdds-pubsub-provider-target.cmake
#
# Injected into every consumer's build by Conan (cmake_build_modules).
#
# Creates a convenience alias:
#   fletcher::fastdds-pubsub-provider  ->  eiva-fletcher-fastdds-pubsub-provider::eiva-fletcher-fastdds-pubsub-provider
#
# Consumers can use either name:
#   target_link_libraries(my_target PRIVATE fletcher::fastdds-pubsub-provider)
#   target_link_libraries(my_target PRIVATE eiva-fletcher-fastdds-pubsub-provider::eiva-fletcher-fastdds-pubsub-provider)

if(TARGET eiva-fletcher-fastdds-pubsub-provider::eiva-fletcher-fastdds-pubsub-provider AND NOT TARGET fletcher::fastdds-pubsub-provider)
    add_library(fletcher::fastdds-pubsub-provider ALIAS eiva-fletcher-fastdds-pubsub-provider::eiva-fletcher-fastdds-pubsub-provider)
endif()
