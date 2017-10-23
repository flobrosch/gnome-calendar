/* gcal-weather-service.h
 *
 * Copyright (C) 2017 - Florian Brosch <flo.brosch@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef GCAL_WEATHER_SERVICE_H
#define GCAL_WEATHER_SERVICE_H

#include <libgweather/gweather.h>
#include <glib-object.h>
#include <glib.h>

#include "gcal-weather-info.h"

G_BEGIN_DECLS

extern const guint GCAL_WEATHER_CHECK_INTERVAL_RENEW_DFLT;
extern const guint GCAL_WEATHER_CHECK_INTERVAL_NEW_DFLT;
extern const guint GCAL_WEATHER_VALID_TIMESPAN_DFLT;
extern const guint GCAL_WEATHER_FORECAST_MAX_DAYS_DFLT;


#define GCAL_TYPE_WEATHER_SERVICE (gcal_weather_service_get_type())

G_DECLARE_FINAL_TYPE (GcalWeatherService, gcal_weather_service, GCAL, WEATHER_SERVICE, GObject)


GcalWeatherService* gcal_weather_service_new                (GTimeZone          *time_zone,
                                                             guint               max_days,
                                                             guint               check_interval_new,
                                                             guint               check_interval_renew,
                                                             gint64              valid_timespan);

GTimeZone*          gcal_weather_service_get_time_zone      (GcalWeatherService *self);

void                gcal_weather_service_set_time_zone      (GcalWeatherService *self,
                                                             GTimeZone          *value);

void                gcal_weather_service_run                (GcalWeatherService *self,
                                                             GWeatherLocation   *location);

void                gcal_weather_service_stop               (GcalWeatherService *self);

guint               gcal_weather_service_get_max_days       (GcalWeatherService *self);

gint64              gcal_weather_service_get_valid_timespan (GcalWeatherService *self);

guint               gcal_weather_service_get_check_interval_new
                                                            (GcalWeatherService *self);

guint               gcal_weather_service_get_check_interval_renew
                                                            (GcalWeatherService *self);

GSList*             gcal_weather_service_get_weather_infos  (GcalWeatherService *self);

const gchar*        gcal_weather_service_get_attribution    (GcalWeatherService *self);

void                gcal_weather_service_update             (GcalWeatherService *self);

G_END_DECLS

#endif /* GCAL_WEATHER_SERVICE_H */

