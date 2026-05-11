# fletcher-xrcedds-pubsub-provider-target.cmake
#
# Injected into every consumer's build by Conan (cmake_build_modules).
#
# Creates a convenience alias:
#   fletcher::xrcedds-pubsub-provider  ->  eiva-fletcher-xrcedds-pubsub-provider::eiva-fletcher-xrcedds-pubsub-provider
#
# Consumers can use either name:
#   target_link_libraries(my_target PRIVATE fletcher::xrcedds-pubsub-provider)
#   target_link_libraries(my_target PRIVATE eiva-fletcher-xrcedds-pubsub-provider::eiva-fletcher-xrcedds-pubsub-provider)

if(TARGET eiva-fletcher-xrcedds-pubsub-provider::eiva-fletcher-xrcedds-pubsub-provider AND NOT TARGET fletcher::xrcedds-pubsub-provider)
    add_library(fletcher::xrcedds-pubsub-provider ALIAS eiva-fletcher-xrcedds-pubsub-provider::eiva-fletcher-xrcedds-pubsub-provider)
endif()
