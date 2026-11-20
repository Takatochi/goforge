#pragma once

/**
 * @file goforge.hpp
 * @brief Umbrella header for the goforge C++20/23 web & application framework built on gorun.
 */

#include "goforge/di.hpp"
#include "goforge/lifecycle.hpp"
#include "goforge/blocking_pool.hpp"
#include "goforge/app.hpp"
#include "goforge/http/http_types.hpp"
#include "goforge/web/context.hpp"
#include "goforge/web/router.hpp"
#include "goforge/web/middleware.hpp"
#include "goforge/obs/logger.hpp"
#include "goforge/obs/metrics.hpp"

#if defined(GOFORGE_HAS_ASIO) && GOFORGE_HAS_ASIO
#include "goforge/net/asio_bridge.hpp"
#include "goforge/http/http_server.hpp"
#endif
