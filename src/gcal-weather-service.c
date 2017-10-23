/* gcal-weather-service.c
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

#define DESKTOP_FILE_NAME         "org.gnome.Calendar"

#include <geocode-glib/geocode-glib.h>
#include <geoclue.h>
#include <string.h>
#include <math.h>

#include "gcal-weather-service.h"
#include "gcal-timer.h"

const guint GCAL_WEATHER_CHECK_INTERVAL_RENEW_DFLT = 3*60*60;
const guint GCAL_WEATHER_CHECK_INTERVAL_NEW_DFLT   = 5*60;
const guint GCAL_WEATHER_VALID_TIMESPAN_DFLT       = 24*60*60;
const guint GCAL_WEATHER_FORECAST_MAX_DAYS_DFLT    = 3;


G_BEGIN_DECLS


/**
 * Internal structure used to manage known
 * weather icons.
 */
typedef struct
{
  gchar     *name;
  gboolean   night_support;
} GcalWeatherIconInfo;


/* GcalWeatherService:
 *
 * @time_zone:               The current time zone
 * @check_interval_new:      Amount of seconds to wait before fetching weather infos.
 * @check_interval_renew:    Amount of seconds to wait before re-fetching weather information.
 * @duration_timer           Timer used to request weather report updates.
 * @midnight_timer           Timer used to update weather reports at midnight.
 * @network_changed_sid      "network-changed" signal ID.
 * @location_service:        Used to monitor location changes.
 *                           Initialized by gcal_weather_service_run(),
 *                           freed by gcal_weather_service_stop().
 * @location_cancellable:    Used to deal with async location service construction.
 * @locaton_running:         Whether location service is active.
 * @weather_infos:           List of #GcalWeatherInfo objects.
 * @weather_infos_upated:    The monotonic time @weather_info was set at.
 * @valid_timespan:          Amount of seconds weather information are considered valid.
 * @gweather_info:           The weather info to query.
 * @max_days:                Number of days we want weather information for.
 * @weather_service_running: True if weather service is active.
 *
 * This service listens to location and weather changes and reports them.
 *
 *  * Create a new instance with gcal_weather_service_new().
 *  * Connect to ::changed to catch weather information.
 *  * Use gcal_weather_service_start() to start the service.
 *  * Use gcal_weather_service_stop() when you are done.
 *
 *  Make sure to stop this service before destroying it.
 */
struct _GcalWeatherService
{
  GObjectClass parent;

  /* <public> */

  /* <private> */
  GTimeZone       *time_zone;            /* owned, nullable */

  /* timer: */
  guint            check_interval_new;
  guint            check_interval_renew;
  GcalTimer       *duration_timer;
  GcalTimer       *midnight_timer;

  /* network monitoring */
  gulong           network_changed_sid;

  /* locations: */
  GClueSimple     *location_service;     /* owned, nullable */
  GCancellable    *location_cancellable; /* owned, non-null */
  gboolean         location_service_running;

  /* weather: */
  GSList          *weather_infos;        /* owned[owned] */
  gint64           weather_infos_upated;
  gint64           valid_timespan;
  GWeatherInfo    *gweather_info;        /* owned, nullable */
  guint            max_days;
  gboolean         weather_service_running;
};



enum
{
  PROP_0,
  PROP_MAX_DAYS,
  PROP_TIME_ZONE,
  PROP_CHECK_INTERVAL_NEW,
  PROP_CHECK_INTERVAL_RENEW,
  PROP_VALID_TIMESPAN,
  PROP_NUM,
};

enum
{
  SIG_WEATHER_CHANGED,
  SIG_NUM,
};

static guint gcal_weather_service_signals[SIG_NUM] = { 0 };


G_DEFINE_TYPE (GcalWeatherService, gcal_weather_service, G_TYPE_OBJECT)


/* Timer Helpers: */
static void     update_timeout_interval                    (GcalWeatherService  *self);

static void     schedule_midnight                          (GcalWeatherService  *self);

static void     start_timer                                (GcalWeatherService  *self);

static void     stop_timer                                 (GcalWeatherService  *self);

static void     on_network_change                          (GNetworkMonitor     *monitor,
                                                            gboolean             available,
                                                            GcalWeatherService  *self);


/* Internal location API and callbacks: */
static void     gcal_weather_service_update_location       (GcalWeatherService  *self,
                                                            GWeatherLocation    *location);

static void     gcal_weather_service_update_gclue_location (GcalWeatherService  *self,
                                                            GClueLocation       *location);

static void     on_gclue_simple_creation                   (GClueSimple         *source,
                                                            GAsyncResult        *result,
                                                            GcalWeatherService  *data);

static void     on_gclue_location_changed                  (GClueLocation       *location,
                                                            GcalWeatherService  *self);

static void     on_gclue_client_activity_changed           (GClueClient         *client,
                                                            GcalWeatherService  *self);

static void     on_gclue_client_stop                       (GClueClient         *client,
                                                            GAsyncResult        *res,
                                                            GClueSimple         *simple);

/* Internal Weather API */
static void     gcal_weather_service_set_max_days          (GcalWeatherService  *self,
                                                            guint                days);

static void     gcal_weather_service_set_valid_timespan    (GcalWeatherService  *self,
                                                            gint64               timespan);

static gboolean has_valid_weather_infos                    (GcalWeatherService  *self);

static void     gcal_weather_service_update_weather        (GcalWeatherService  *self,
                                                            GWeatherInfo        *info,
                                                            gboolean             reuse_old_on_error);

static void     on_gweather_update                         (GWeatherInfo        *info,
                                                            GcalWeatherService  *self);

/* Internal weather update timer API and callbacks */
static void     gcal_weather_service_set_check_interval_new   (GcalWeatherService *self,
                                                               guint               check_interval);

static void     gcal_weather_service_set_check_interval_renew (GcalWeatherService *self,
                                                               guint               check_interval);

static gssize   get_normalized_icon_name_len               (const gchar         *str);

static gchar*   get_normalized_icon_name                   (GWeatherInfo        *gwi,
                                                            gboolean             is_night_icon);

static gint     get_icon_name_sortkey                      (const gchar         *icon_name,
                                                            gboolean            *supports_night_icon);

static void     on_duration_timer_timeout                  (GcalTimer           *timer,
                                                            GcalWeatherService  *self);

static void     on_midnight_timer_timeout                  (GcalTimer           *timer,
                                                            GcalWeatherService  *self);

static gboolean get_time_day_start                         (GcalWeatherService  *self,
                                                            GDate               *ret_date,
                                                            gint64              *ret_unix,
                                                            gint64              *ret_unix_exact);

static inline gboolean get_gweather_temperature            (GWeatherInfo        *gwi,
                                                            gdouble             *temp);

static gboolean compute_weather_info_data                  (GSList              *samples,
                                                            gboolean             is_today,
                                                            gchar              **icon_name,
                                                            gchar              **temperature);

static GSList*  preprocess_gweather_reports                (GcalWeatherService  *self,
                                                            GSList              *samples);


G_END_DECLS


/********************
 * < gobject setup >
 *******************/

static void
gcal_weather_service_finalize (GObject *object)
{
  GcalWeatherService *self; /* unowned */

  self = (GcalWeatherService *) object;

  gcal_timer_free (self->duration_timer);
  self->duration_timer = NULL;

  gcal_timer_free (self->midnight_timer);
  self->midnight_timer = NULL;

  if (self->time_zone != NULL)
    {
      g_time_zone_unref (self->time_zone);
      self->time_zone = NULL;
    }

  if (self->location_service != NULL)
    g_clear_object (&self->location_service);

  g_cancellable_cancel (self->location_cancellable);
  g_clear_object (&self->location_cancellable);

  g_slist_free_full (self->weather_infos, g_object_unref);

  if (self->gweather_info != NULL)
    g_clear_object (&self->gweather_info);

  if (self->network_changed_sid > 0)
    {
      GNetworkMonitor *monitor; /* unowned */

      monitor = g_network_monitor_get_default ();
      g_signal_handler_disconnect (monitor, self->network_changed_sid);
    }

  G_OBJECT_CLASS (gcal_weather_service_parent_class)->finalize (object);
}



static void
gcal_weather_service_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  GcalWeatherService* self; /* unowned */

  self = GCAL_WEATHER_SERVICE (object);
  switch (prop_id)
  {
  case PROP_MAX_DAYS:
    g_value_set_uint (value, gcal_weather_service_get_max_days (self));
    break;
  case PROP_TIME_ZONE:
    g_value_set_pointer (value, gcal_weather_service_get_time_zone (self));
    break;
  case PROP_CHECK_INTERVAL_NEW:
    g_value_set_uint (value, gcal_weather_service_get_check_interval_new (self));
    break;
  case PROP_CHECK_INTERVAL_RENEW:
    g_value_set_uint (value, gcal_weather_service_get_check_interval_renew (self));
    break;
  case PROP_VALID_TIMESPAN:
    g_value_set_int64 (value, gcal_weather_service_get_valid_timespan (self));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    break;
  }
}



static void
gcal_weather_service_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  GcalWeatherService* self; /* unowned */

  self = GCAL_WEATHER_SERVICE (object);
  switch (prop_id)
  {
  case PROP_MAX_DAYS:
    gcal_weather_service_set_max_days (self, g_value_get_uint (value));
    break;
  case PROP_TIME_ZONE:
    gcal_weather_service_set_time_zone (self, g_value_get_pointer (value));
    break;
  case PROP_CHECK_INTERVAL_NEW:
    gcal_weather_service_set_check_interval_new (self, g_value_get_uint (value));
    break;
  case PROP_CHECK_INTERVAL_RENEW:
    gcal_weather_service_set_check_interval_renew (self, g_value_get_uint (value));
    break;
  case PROP_VALID_TIMESPAN:
    gcal_weather_service_set_valid_timespan (self, g_value_get_int64 (value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    break;
  }
}



static void
gcal_weather_service_class_init (GcalWeatherServiceClass *klass)
{
  GObjectClass *object_class; /* unowned */

  object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gcal_weather_service_finalize;
  object_class->get_property = gcal_weather_service_get_property;
  object_class->set_property = gcal_weather_service_set_property;

  /**
   * GcalWeatherService:max-days:
   *
   * Maximal number of days to fetch forecasts for.
   */
  g_object_class_install_property
      (G_OBJECT_CLASS (klass),
       PROP_MAX_DAYS,
       g_param_spec_uint ("max-days", "max-days", "max-days",
                          1, G_MAXUINT, 3,
                          G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  /**
   * GcalWeatherService:time-zone:
   *
   * The time zone to use.
   */
  g_object_class_install_property
      (G_OBJECT_CLASS (klass),
       PROP_TIME_ZONE,
       g_param_spec_pointer ("time-zone", "time-zone", "time-zone",
                             G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));

  /**
   * GcalWeatherService:check-interval-new:
   *
   * Amount of seconds to wait before re-fetching weather infos.
   * This interval is used when no valid weather reports are available.
   */
  g_object_class_install_property
      (G_OBJECT_CLASS (klass),
       PROP_CHECK_INTERVAL_NEW,
       g_param_spec_uint ("check-interval-new", "check-interval-new", "check-interval-new",
                          0, G_MAXUINT, GCAL_WEATHER_CHECK_INTERVAL_NEW_DFLT,
                          G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  /**
   * GcalWeatherService:check-interval-renew:
   *
   * Amount of seconds to wait before re-fetching weather information.
   * This interval is used when valid weather reports are available.
   */
  g_object_class_install_property
      (G_OBJECT_CLASS (klass),
       PROP_CHECK_INTERVAL_RENEW,
       g_param_spec_uint ("check-interval-renew", "check-interval-renew", "check-interval-renew",
                          0, G_MAXUINT, GCAL_WEATHER_CHECK_INTERVAL_RENEW_DFLT,
                          G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  /**
   * GcalWeatherService:valid_timespan:
   *
   * Amount of seconds fetched weather information are considered as valid.
   */
  g_object_class_install_property
      (G_OBJECT_CLASS (klass),
       PROP_VALID_TIMESPAN,
       g_param_spec_int64 ("valid-timespan", "valid-timespan", "valid-timespan",
                           0, G_MAXINT64, GCAL_WEATHER_VALID_TIMESPAN_DFLT,
                           G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));


  /**
   * GcalWeatherService::weather-changed:
   * @sender: The #GcalWeatherService
   * @self:   data pointer.
   *
   * Triggered on weather changes. Call
   * gcal_weather_service_get_weather_infos() to
   * retrieve predictions.
   */
  gcal_weather_service_signals[SIG_WEATHER_CHANGED]
    = g_signal_new ("weather-changed",
                    GCAL_TYPE_WEATHER_SERVICE,
                    G_SIGNAL_RUN_LAST,
                    0,
                    NULL,
                    NULL,
                    g_cclosure_marshal_VOID__VOID,
                    G_TYPE_NONE,
                    0);
}



static void
gcal_weather_service_init (GcalWeatherService *self)
{
  GNetworkMonitor *monitor; /* unowned */

  self->duration_timer = gcal_timer_new (GCAL_WEATHER_CHECK_INTERVAL_NEW_DFLT);
  gcal_timer_set_callback (self->duration_timer, (GCalTimerFunc) on_duration_timer_timeout, self, NULL);
  self->midnight_timer = gcal_timer_new (24*60*60);
  gcal_timer_set_callback (self->midnight_timer, (GCalTimerFunc) on_midnight_timer_timeout, self, NULL);
  self->time_zone = NULL;
  self->check_interval_new = GCAL_WEATHER_CHECK_INTERVAL_NEW_DFLT;
  self->check_interval_renew = GCAL_WEATHER_CHECK_INTERVAL_RENEW_DFLT;
  self->location_cancellable = g_cancellable_new ();
  self->location_service_running = FALSE;
  self->location_service = NULL;
  self->weather_infos = NULL;
  self->weather_infos_upated = -1;
  self->valid_timespan = -1;
  self->gweather_info = NULL;
  self->weather_service_running = FALSE;
  self->max_days = 0;

  monitor = g_network_monitor_get_default ();
  self->network_changed_sid = g_signal_connect (monitor,
                                                "network-changed",
                                                (GCallback) on_network_change,
                                                self);
}



/* drop_suffix:
 * @str: A icon name to normalize.
 *
 * Translates a given weather icon name to the
 * one to display for folded weather reports.
 *
 * Returns: Number of initial characters that
 *          belong to the normalized name.
 */
static gssize
get_normalized_icon_name_len (const gchar *str)
{
  const gchar suffix1[] = "-symbolic";
  const gssize suffix1_len = G_N_ELEMENTS (suffix1) - 1;

  const gchar suffix2[] = "-night";
  const gssize suffix2_len = G_N_ELEMENTS (suffix2) - 1;

  gssize clean_len;
  gssize str_len;

  g_return_val_if_fail (str != NULL, -1);

  str_len = strlen (str);

  clean_len = str_len - suffix1_len;
  if (clean_len >= 0 && memcmp (suffix1, str + clean_len, suffix1_len) == 0)
    str_len = clean_len;

  clean_len = str_len - suffix2_len;
  if (clean_len >= 0 && memcmp (suffix2, str + clean_len, suffix2_len) == 0)
    str_len = clean_len;

  return str_len;
}



/* get_normalized_icon_name:
 * @str: A icon name to normalize.
 *
 * Translates a given weather icon name to the
 * one to display for folded weather reports.
 *
 * Returns: (transfer full): A normalized icon name.
 */
static gchar*
get_normalized_icon_name (GWeatherInfo* wi,
                          gboolean      is_night_icon)
{
  const gchar night_pfx[] = "-night";
  const gsize night_pfx_size = G_N_ELEMENTS (night_pfx) - 1;

  const gchar sym_pfx[] = "-symbolic";
  const gsize sym_pfx_size = G_N_ELEMENTS (sym_pfx) - 1;

  const gchar *str; /* unowned */
  gssize       normalized_size;
  gchar       *buffer = NULL; /* owned */
  gchar       *bufpos = NULL; /* unowned */
  gsize        buffer_size;

  g_return_val_if_fail (wi != NULL, NULL);
  
  str = gweather_info_get_icon_name (wi);
  if (str == NULL)
    return NULL;
  
  normalized_size = get_normalized_icon_name_len (str);
  g_return_val_if_fail (normalized_size >= 0, NULL);

  if (is_night_icon) 
    buffer_size = normalized_size + night_pfx_size + sym_pfx_size + 1;
  else
    buffer_size = normalized_size + sym_pfx_size + 1;

  buffer = g_malloc (buffer_size);
  bufpos = buffer;

  memcpy (bufpos, str, normalized_size);
  bufpos = bufpos + normalized_size;

  if (is_night_icon)
    {
      memcpy (bufpos, night_pfx, night_pfx_size);
      bufpos = bufpos + night_pfx_size;
    }

  memcpy (bufpos, sym_pfx, sym_pfx_size);
  buffer[buffer_size - 1] = '\0';  

  return buffer;
}



/* get_icon_name_sortkey:
 *
 * Returns a sort key for a given weather
 * icon name and -1 for unknown ones.
 *
 * The lower the key, the better the weather.
 */
static gint
get_icon_name_sortkey (const gchar *icon_name,
                       gboolean    *supports_night_icon)
{
  /* Note that we can't use gweathers condition
   * field as it is not necessarily holds valid values.
   * libgweather uses its own heuristic to determine
   * the icon to use. String matching is still better
   * than copying their algorithm, I guess.
   */

  gssize normalized_name_len;

  const GcalWeatherIconInfo icons[] =
    { {"weather-clear",             TRUE},
      {"weather-few-clouds",        TRUE},
      {"weather-overcast",          FALSE},
      {"weather-fog",               FALSE},
      {"weather-showers-scattered", FALSE},
      {"weather-showers",           FALSE},
      {"weather-snow",              FALSE},
      {"weather-storm",             FALSE},
      {"weather-severe-alert",      FALSE}
    };

  g_return_val_if_fail (icon_name != NULL, -1);
  g_return_val_if_fail (supports_night_icon != NULL, -1);

  *supports_night_icon = FALSE;

  normalized_name_len = get_normalized_icon_name_len (icon_name);
  g_return_val_if_fail (normalized_name_len >= 0, -1);

  for (int i = 0; i < G_N_ELEMENTS (icons); i++)
    {
      if (icons[i].name[normalized_name_len] == '\0' && strncmp (icon_name, icons[i].name, normalized_name_len) == 0)
        {
          *supports_night_icon = icons[i].night_support;
          return i;
        }
    }

  g_warning ("Unknown weather icon '%s'", icon_name);

  return -1;
}



/**************
 * < private >
 **************/

#if PRINT_WEATHER_DATA
static gchar*
gwc2str (GWeatherInfo *gwi)
{
    g_autoptr (GDateTime) date = NULL;
    g_autofree gchar     *date_str = NULL;
    glong      update;

    gchar     *icon_name; /* unowned */
    gdouble    temp;

    if (!gweather_info_get_value_update (gwi, &update))
        return g_strdup ("<null>");

    date = g_date_time_new_from_unix_local (update);
    date_str = g_date_time_format (date, "%F %T"),

    get_gweather_temperature (gwi, &temp);
    icon_name = gweather_info_get_symbolic_icon_name (gwi);

    return g_strdup_printf ("(%s: t:%f, w:%s)",
                            date_str,
                            temp,
                            icon_name);
}
#endif



/* get_time_day_start:
 * @self: The #GcalWeatherService instance.
 * @date: (out) (not nullable): A #GDate that should be set to today.
 * @unix: (out) (not nullable): A UNIX time stamp that should be set to today.
 * @ret_unix_exact (out) (not nullable): The exact date time this data point predicts.
 *
 * Provides current date in two different forms.
 *
 * Returns: %TRUE on success.
 */
static gboolean
get_time_day_start (GcalWeatherService *self,
                    GDate              *ret_date,
                    gint64             *ret_unix,
                    gint64             *ret_unix_exact)
{
  g_autoptr (GTimeZone) zone = NULL;
  g_autoptr (GDateTime) now = NULL;
  g_autoptr (GDateTime) day = NULL;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (ret_date != NULL, FALSE);
  g_return_val_if_fail (ret_unix != NULL, FALSE);
  g_return_val_if_fail (ret_unix_exact != NULL, FALSE);

  zone = (self->time_zone == NULL)
          ? g_time_zone_new_local ()
          : g_time_zone_ref (self->time_zone);

  now = g_date_time_new_now (zone);
  day = g_date_time_new (zone,
                         g_date_time_get_year (now),
                         g_date_time_get_month (now),
                         g_date_time_get_day_of_month (now),
                         0, 0, 0);

  g_date_set_dmy (ret_date,
                  g_date_time_get_day_of_month (day),
                  g_date_time_get_month (day),
                  g_date_time_get_year (day));

  *ret_unix = g_date_time_to_unix (day);
  *ret_unix_exact = g_date_time_to_unix (now);
  return TRUE;
}



/* get_gweather_temperature:
 * @gwi: #GWeatherInfo to extract temperatures from.
 * @temp: (out): extracted temperature or %NAN.
 *
 * Returns sanitized temperatures. Returned values should only
 * be used as sort key.
 *
 * Returns: %TRUE for valid temperatures.
 */
static inline gboolean
get_gweather_temperature (GWeatherInfo *gwi,
                          gdouble      *temp)
{
  gboolean valid;
  gdouble  value;

  *temp = NAN;

  g_return_val_if_fail (gwi != NULL, FALSE);
  g_return_val_if_fail (temp != NULL, FALSE);

  valid = gweather_info_get_value_temp (gwi,
                                        GWEATHER_TEMP_UNIT_DEFAULT,
                                        &value);

  /* TODO: Extract temperatures in Celsius and catch
   *       implausible cases.
   */
  if (valid)
    {
      *temp = value;
      return TRUE;
    }
  else
    {
      *temp = NAN;
      return FALSE;
    }
}



/* compute_weather_info_data:
 * @samples: List of received #GWeatherInfos.
 * @icon_name: (out): (transfer full): weather icon name or %NULL.
 * @temperature: (out): (transfer full): temperature and unit or %NULL.
 *
 * Computes a icon name and temperature representing @samples.
 *
 * Returns: %TRUE if there is a valid icon name and temperature.
 */
static gboolean
compute_weather_info_data (GSList    *samples,
                           gboolean   is_today,
                           gchar    **icon_name,
                           gchar    **temperature)
{
  gboolean      phenomenon_supports_night_icon = FALSE;
  gint          phenomenon_val = -1;
  GWeatherInfo *phenomenon_gwi = NULL; /* unowned */
  gdouble       temp_val = NAN;
  GWeatherInfo *temp_gwi = NULL; /* unowned */
  gboolean      has_daytime = FALSE;
  GSList       *iter;            /* unowned */

  /* Note: I checked three different gweather consumers
   *   and they all pick different values. So here is my
   *   take: I pick up the worst weather for icons and
   *   the highest temperature. I basically want to know
   *   whether I need my umbrella for my appointment.
   *   Not sure about the right temperature. It is probably
   *   better to pick-up the median of all predictions
   *   during daytime.
   */

  g_return_val_if_fail (icon_name != NULL, FALSE);
  g_return_val_if_fail (temperature != NULL, FALSE);

  for (iter = samples; iter != NULL; iter = iter->next)
    {
      GWeatherInfo  *gwi;       /* unowned */
      const gchar   *icon_name; /* unowned */
      gint           phenomenon = -1;
      gboolean       supports_night_icon;
      gdouble  temp;
      gboolean valid_temp;

      gwi = GWEATHER_INFO (iter->data);

      icon_name = gweather_info_get_icon_name (gwi);
      if (icon_name != NULL)
        phenomenon  = get_icon_name_sortkey (icon_name, &supports_night_icon);

      valid_temp = get_gweather_temperature (gwi, &temp);

      if (phenomenon >= 0 && (phenomenon_gwi == NULL || phenomenon > phenomenon_val))
        {
          phenomenon_supports_night_icon = supports_night_icon;
          phenomenon_val = phenomenon;
          phenomenon_gwi = gwi;
        }

      if (valid_temp && (temp_gwi == NULL || temp > temp_val))
        {
          temp_val = temp;
          temp_gwi = gwi;
        }

      if (gweather_info_is_daytime (gwi))
        has_daytime = TRUE;
    }

  if (phenomenon_gwi != NULL && temp_gwi != NULL)
    {
      *icon_name = get_normalized_icon_name (phenomenon_gwi, is_today && !has_daytime && phenomenon_supports_night_icon);
      *temperature = gweather_info_get_temp (temp_gwi);
      return TRUE;
    }
  else
    {
      /* empty list */
      *icon_name = NULL;
      *temperature = NULL;
      return FALSE;
    }
}



/* preprocess_gweather_reports:
 * @self:     The #GcalWeatherService instance.
 * @samples:  Received list of #GWeatherInfos
 *
 * Computes weather info objects representing specific days
 * by combining given #GWeatherInfos.
 *
 * Returns: (transfer full): List of up to $self->max_days #GcalWeatherInfos.
 */
static GSList*
preprocess_gweather_reports (GcalWeatherService *self,
                             GSList             *samples)
{
  const glong DAY_SECONDS = 24*60*60;
  GSList  *result = NULL; /* owned[owned] */
  GSList **days;          /* owned[owned[unowned]] */
  GSList *iter;           /* unowned */
  GDate cur_gdate;
  glong today_unix;
  glong unix_now;

  GWeatherInfo *first_tomorrow = NULL; /* unowned */
  glong         first_tomorrow_dtime = -1;

  g_return_val_if_fail (GCAL_IS_WEATHER_SERVICE (self), NULL);

  /* This function basically separates samples by
   * date and calls compute_weather_info_data for
   * every bucket to build weather infos.
   * Each bucket represents a single day.
   */

  /* All gweather consumers I reviewed presume
   * sorted samples. However, there is no documented
   * order. Lets assume the worst.
   */

  if (self->max_days <= 0)
    return NULL;

  if (!get_time_day_start (self, &cur_gdate, &today_unix, &unix_now))
    return NULL;

  days = g_malloc0 (sizeof (GSList*) * self->max_days);

  /* Split samples to max_days buckets: */
  for (iter = samples; iter != NULL; iter = iter->next)
    {
      GWeatherInfo *gwi; /* unowned */
      gboolean valid_date;
      glong gwi_dtime;
      gsize bucket;

      gwi = GWEATHER_INFO (iter->data);
      valid_date = gweather_info_get_value_update (gwi, &gwi_dtime);
      if (!valid_date)
        continue;

      #if PRINT_WEATHER_DATA
      {
        g_autofree gchar* dbg_str = gwc2str (gwi);
        g_message ("WEATHER READING POINT: %s", dbg_str);
      }
      #endif

      if (gwi_dtime >= 0 && gwi_dtime >= today_unix)
        {
          bucket = (gwi_dtime - today_unix) / DAY_SECONDS;
          if (bucket >= 0 && bucket < self->max_days)
            days[bucket] = g_slist_prepend (days[bucket], gwi);

          if (bucket == 1 && (first_tomorrow == NULL || first_tomorrow_dtime > gwi_dtime))
            {
              first_tomorrow_dtime = gwi_dtime;
              first_tomorrow = gwi;
            }
        }
      else
        {
          g_debug ("Encountered historic weather information");
        }
    }

  if (days[0] == NULL && first_tomorrow != NULL)
    {
        /* There is no data point left for today.
         * Lets borrow one.
         */
       glong secs_left_today;
       glong secs_between;

       secs_left_today = DAY_SECONDS - (unix_now - today_unix);
       secs_between = first_tomorrow_dtime - unix_now;

       if (secs_left_today < 90*60 && secs_between <= 180*60)
         days[0] = g_slist_prepend (days[0], first_tomorrow);
    }

  /* Produce GcalWeatherInfo for each bucket: */
  for (int i = 0; i < self->max_days; i++)
    {
      g_autofree gchar *icon_name;
      g_autofree gchar *temperature;

      if (compute_weather_info_data (days[i], i == 0, &icon_name, &temperature))
        {
          GcalWeatherInfo* gcwi; /* owned */

          gcwi = gcal_weather_info_new (&cur_gdate,
                                        icon_name,
                                        temperature);

          result = g_slist_prepend (result,
                                    g_steal_pointer (&gcwi));
        }

      g_date_add_days (&cur_gdate, 1);
    }

  /* Cleanup: */
  for (int i = 0; i < self->max_days; i++)
    g_slist_free (days[i]);
  g_free (days);

  return result;
}



/**
 * gcal_weather_service_update_location:
 * @self:     The #GcalWeatherService instance.
 * @location: (nullable): The location we want weather information for.
 *
 * Registers the location to retrieve weather information from.
 */
static void
gcal_weather_service_update_location (GcalWeatherService  *self,
                                      GWeatherLocation    *location)
{
  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));

  if (gcal_timer_is_running (self->duration_timer))
    stop_timer (self);

  if (self->gweather_info != NULL)
    g_clear_object (&self->gweather_info);

  if (location == NULL)
    {
      g_debug ("Could not retrieve current location");
      gcal_weather_service_update_weather (self, NULL, FALSE);
    }
  else
    {
      g_debug ("Got new weather service location: '%s'",
               (location == NULL)? "<null>" : gweather_location_get_name (location));

      self->gweather_info = gweather_info_new (location, GWEATHER_FORECAST_ZONE | GWEATHER_FORECAST_LIST);

      /* NOTE: We do not get detailed infos for GWEATHER_PROVIDER_ALL.
       * This combination works fine, though. We should open a bug / investigate
       * what is going on.
       */
      gweather_info_set_enabled_providers (self->gweather_info, GWEATHER_PROVIDER_METAR | GWEATHER_PROVIDER_OWM | GWEATHER_PROVIDER_YR_NO);
      g_signal_connect (self->gweather_info, "updated", (GCallback) on_gweather_update, self);

      /* gweather_info_update might or might not trigger a
       * GWeatherInfo::update() signal. Therefore, we have to
       * remove weather information before querying new one.
       * This might result in icon flickering on screen.
       * We probably want to introduce a "unknown" or "loading"
       * state in gweather-info to soften the effect.
       */
      gcal_weather_service_update_weather (self, NULL, FALSE);
      gweather_info_update (self->gweather_info);

      start_timer (self);
    }
}



/**
 * gcal_weather_service_update_gclue_location:
 * @self:     The #GcalWeatherService instance.
 * @location: (nullable): The location we want weather information for.
 *
 * Registers the location to retrieve weather information from.
 */
static void
gcal_weather_service_update_gclue_location (GcalWeatherService  *self,
                                            GClueLocation       *location)
{
  GWeatherLocation *wlocation = NULL; /* owned */

  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));
  g_return_if_fail (location == NULL || GCLUE_IS_LOCATION (location));

  if (location != NULL)
    {
      GWeatherLocation *wworld; /* unowned */
      gdouble latitude;
      gdouble longitude;

      latitude = gclue_location_get_latitude (location);
      longitude = gclue_location_get_longitude (location);

      /* nearest-city works more closely to gnome weather. */
      wworld = gweather_location_get_world ();
      wlocation = gweather_location_find_nearest_city (wworld, latitude, longitude);
    }


  gcal_weather_service_update_location (self, wlocation);

  if (wlocation != NULL)
    gweather_location_unref (wlocation);
}



/* on_gclue_simple_creation:
 * @source:
 * @result:                Result of gclue_simple_new().
 * @self: (transfer full): A GcalWeatherService reference.
 *
 * Callback used in gcal_weather_service_run().
 */
static void
on_gclue_simple_creation (GClueSimple        *_source,
                          GAsyncResult       *result,
                          GcalWeatherService *self)
{
  GClueSimple *location_service;   /* owned */
  GClueLocation *location;         /* unowned */
  GClueClient *client;             /* unowned */
  g_autoptr (GError) error = NULL;

  g_return_if_fail (G_IS_ASYNC_RESULT (result));
  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));

  /* make sure we do not touch self->location_service
   * if the current operation was cancelled.
   */
  location_service = gclue_simple_new_finish (result, &error);
  if (error != NULL)
    {
      g_assert (location_service == NULL);

      if (error->domain == G_IO_ERROR && error->code == G_IO_ERROR_CANCELLED)
        /* Cancelled during creation. Silently fail. */;
      else
        g_warning ("Could not create GCLueSimple: %s", error->message);

      g_object_unref (self);
      return;
    }

  g_assert (self->location_service == NULL);
  g_assert (location_service != NULL);

  self->location_service = g_steal_pointer (&location_service);

  location = gclue_simple_get_location (self->location_service);
  client = gclue_simple_get_client (self->location_service);

  if (location != NULL)
    {
      gcal_weather_service_update_gclue_location (self, location);

      g_signal_connect_object (location,
                               "notify::location",
                               G_CALLBACK (on_gclue_location_changed),
                               self,
                               0);
    }

  g_signal_connect_object (client,
                           "notify::active",
                           G_CALLBACK (on_gclue_client_activity_changed),
                           self,
                           0);

  g_object_unref (self);
}


/* on_gclue_location_changed:
 * @location: #GClueLocation owned by @self
 * @self: The #GcalWeatherService
 *
 * Handles location changes.
 */
static void
on_gclue_location_changed (GClueLocation       *location,
                           GcalWeatherService  *self)
{
  g_return_if_fail (GCLUE_IS_LOCATION (location));
  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));

  gcal_weather_service_update_gclue_location (self, location);
}



/* on_gclue_client_activity_changed:
 * @client: The #GClueclient ownd by @self
 * @self: The #GcalWeatherService
 *
 * Handles location client activity changes.
 */
static void
on_gclue_client_activity_changed (GClueClient         *client,
                                  GcalWeatherService  *self)
{
  g_return_if_fail (GCLUE_IS_CLIENT (client));
  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));

  /* Notify listeners about unknown locations: */
  gcal_weather_service_update_location (self, NULL);
}



/* on_gclue_client_stop:
 * @source_object: A #GClueClient.
 * @res:           Result of gclue_client_call_stop().
 * @simple:        (transfer full): A #GClueSimple.
 *
 * Helper-callback used in gcal_weather_service_stop().
 */
static void
on_gclue_client_stop (GClueClient  *client,
                      GAsyncResult *res,
                      GClueSimple  *simple)
{
  g_autoptr(GError) error = NULL; /* owned */
  gboolean stopped;

  g_return_if_fail (GCLUE_IS_CLIENT (client));
  g_return_if_fail (G_IS_ASYNC_RESULT (res));
  g_return_if_fail (GCLUE_IS_SIMPLE (simple));

  stopped = gclue_client_call_stop_finish (client,
                                           res,
                                           &error);
  if (error != NULL)
      g_warning ("Could not stop location service: %s", error->message);
  else if (!stopped)
      g_warning ("Could not stop location service");

  g_object_unref (simple);
}



/* gcal_weather_service_set_max_days:
 * @self: The #GcalWeatherService instance.
 * @days: Number of days.
 *
 * Setter for #GcalWeatherInfos:max-days.
 */
static void
gcal_weather_service_set_max_days (GcalWeatherService *self,
                                   guint               days)
{
  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));
  g_return_if_fail (days >= 1);

  self->max_days = days;

  g_object_notify ((GObject*) self, "max-days");
}



/* gcal_weather_service_set_valid_timespan:
 * @self: A #GcalWeatherService instance.
 * @timespan: Amount of seconds we consider weather information as valid.
 */
static void
gcal_weather_service_set_valid_timespan (GcalWeatherService *self,
                                         gint64              timespan)
{
  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));
  g_return_if_fail (timespan >= 0);

  self->valid_timespan = timespan;

  g_object_notify ((GObject*) self, "valid-timespan");
}



/* has_valid_weather_infos
 * @self: A #GcalWeatherService instance.
 *
 * Checks whether weather information are available
 * and up-to-date.
 */
static gboolean
has_valid_weather_infos (GcalWeatherService *self)
{
  gint64 now;

  g_return_val_if_fail (GCAL_IS_WEATHER_SERVICE (self), FALSE);

  if (self->gweather_info == NULL || self->weather_infos_upated < 0)
    return FALSE;

  now = g_get_monotonic_time ();
  return (now - self->weather_infos_upated) / 1000000 <= self->valid_timespan;
}



/* gcal_weather_service_update_weather:
 * @self: A #GcalWeatherService instance.
 * @info: (nullable): Newly received weather information or %NULL.
 * @reuse_old_on_error: Whether to re-use old but not outdated weather
 *                      information in case we could not fetch new data.
 *
 * Retrieves weather information for @location and triggers
 * #GcalWeatherService::weather-changed.
 */
static void
gcal_weather_service_update_weather (GcalWeatherService *self,
                                     GWeatherInfo       *info,
                                     gboolean            reuse_old_on_error)
{
  GSList *gwforecast = NULL; /* unowned */

  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));
  g_return_if_fail (info == NULL || GWEATHER_IS_INFO (info));


  /* Compute a list of newly received weather infos. */
  if (info == NULL)
    {
      g_debug ("Could not retrieve valid weather");
    }
  else if (gweather_info_is_valid (info))
    {
      g_debug ("Received valid weather information");
      gwforecast = gweather_info_get_forecast_list (info);
    }
  else
    {
      g_autofree gchar* location_name = gweather_info_get_location_name (info);
      g_debug ("Could not retrieve valid weather for location '%s'", location_name);
    }

  if (gwforecast == NULL && self->weather_infos_upated >= 0)
    {
      if (!reuse_old_on_error || !has_valid_weather_infos (self))
        {
          g_slist_free_full (self->weather_infos, g_object_unref);
          self->weather_infos = NULL;
          self->weather_infos_upated = -1;

          g_signal_emit (self, gcal_weather_service_signals[SIG_WEATHER_CHANGED], 0);
        }
    }
  else if (gwforecast != NULL)
    {
      g_slist_free_full (self->weather_infos, g_object_unref);
      self->weather_infos = preprocess_gweather_reports (self, gwforecast);
      self->weather_infos_upated = g_get_monotonic_time ();

      g_signal_emit (self, gcal_weather_service_signals[SIG_WEATHER_CHANGED], 0);
    }
}



/* on_gweather_update:
 * @self: A #GcalWeatherService instance.
 * @timespan: Amount of seconds we consider weather information as valid.
 *
 * Triggered on weather updates with previously handled or no
 * location changes.
 */
static void
on_gweather_update (GWeatherInfo       *info,
                    GcalWeatherService *self)
{
  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));
  g_return_if_fail (info == NULL || GWEATHER_IS_INFO (info));

  gcal_weather_service_update_weather (self, info, TRUE);
}



/* update_timeout_interval:
 * @self: The #GcalWeatherService instance.
 *
 * Selects the right duration timer timeout based
 * on locally-stored weather information.
 */
static void
update_timeout_interval (GcalWeatherService *self)
{
  guint interval;

  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));

  if (has_valid_weather_infos (self))
    interval = self->check_interval_renew;
  else
    interval = self->check_interval_new;

  gcal_timer_set_default_duration (self->duration_timer, interval);
}



/* schedule_midnight:
 * @self: The #GcalWeatherService instance.
 *
 * Sets the midnight timer timeout to midnight.
 * The timer needs to be reset when it
 * emits.
 */
static void
schedule_midnight (GcalWeatherService  *self)
{
  g_autoptr (GTimeZone) zone = NULL;
  g_autoptr (GDateTime) now = NULL;
  g_autoptr (GDateTime) tom = NULL;
  g_autoptr (GDateTime) mid = NULL;
  gint64 real_now;
  gint64 real_mid;

  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));

  zone = (self->time_zone == NULL)
           ? g_time_zone_new_local ()
           : g_time_zone_ref (self->time_zone);

  now = g_date_time_new_now (zone);
  tom = g_date_time_add_days (now, 1);
  mid = g_date_time_new (zone,
                         g_date_time_get_year (tom),
                         g_date_time_get_month (tom),
                         g_date_time_get_day_of_month (tom),
                         0, 0, 0);

  real_mid = g_date_time_to_unix (mid);
  real_now = g_date_time_to_unix (now);

  gcal_timer_set_default_duration (self->midnight_timer,
                                   real_mid - real_now);
}



/* start_timer:
 * @self: The #GcalWeatherService instance.
 *
 * Starts weather timer in case it makes sense.
 */
static void
start_timer (GcalWeatherService  *self)
{
  GNetworkMonitor *monitor; /* unowned */

  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));

  monitor = g_network_monitor_get_default ();
  if (g_network_monitor_get_network_available (monitor))
    {
      update_timeout_interval (self);
      gcal_timer_start (self->duration_timer);

      schedule_midnight (self);
      gcal_timer_start (self->duration_timer);
    }
}



/* stop_timer:
 * @self: The #GcalWeatherService instance.
 *
 * Stops the timer.
 */
static void
stop_timer (GcalWeatherService  *self)
{
  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));

  gcal_timer_stop (self->duration_timer);
  gcal_timer_stop (self->midnight_timer);
}



/* on_network_change:
 * @monitor:   The emitting #GNetworkMonitor
 * @available: The current value of “network-available”
 * @self:      The #GcalWeatherService instance.
 *
 * Starts and stops timer based on monitored network
 * changes.
 */
static void
on_network_change (GNetworkMonitor    *monitor,
                   gboolean            available,
                   GcalWeatherService *self)
{
  gboolean is_running;

  g_return_if_fail (G_IS_NETWORK_MONITOR (monitor));
  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));

  g_debug ("network changed, available = %d", available);

  is_running = gcal_timer_is_running (self->duration_timer);
  if (available && !is_running)
    {
      if (self->gweather_info != NULL)
        gweather_info_update (self->gweather_info);

      start_timer (self);
    }
  else if (!available && is_running)
    {
      stop_timer (self);
    }
}



/* gcal_weather_service_set_check_interval_new:
 * @self: The #GcalWeatherService instance.
 * @days: Number of days.
 *
 * Setter for GcalWeatherInfos:check-interval-new.
 */
static void
gcal_weather_service_set_check_interval_new (GcalWeatherService *self,
                                             guint               interval)
{
  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));
  g_return_if_fail (interval > 0);

  self->check_interval_new = interval;
  update_timeout_interval (self);

  g_object_notify ((GObject*) self, "check-interval-new");
}



/* gcal_weather_service_set_check_interval_renew:
 * @self: The #GcalWeatherService instance.
 * @days: Number of days.
 *
 * Setter for GcalWeatherInfos:check-interval-renew.
 */
static void
gcal_weather_service_set_check_interval_renew (GcalWeatherService *self,
                                               guint               interval)
{
  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));
  g_return_if_fail (interval > 0);

  self->check_interval_renew = interval;
  update_timeout_interval (self);

  g_object_notify ((GObject*) self, "check-interval-renew");
}



/* on_duration_timer_timeout
 * @self: A #GcalWeatherService.
 *
 * Handles scheduled weather report updates.
 */
static void
on_duration_timer_timeout (GcalTimer          *timer,
                           GcalWeatherService *self)
{
  g_return_if_fail (timer != NULL);
  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));

  if (self->gweather_info != NULL)
    gweather_info_update (self->gweather_info);
}



/* on_midnight_timer_timeout
 * @self: A #GcalWeatherService.
 *
 * Handles scheduled weather report updates.
 */
static void
on_midnight_timer_timeout (GcalTimer          *timer,
                           GcalWeatherService *self)
{
  g_return_if_fail (timer != NULL);
  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));

  if (self->gweather_info != NULL)
    gweather_info_update (self->gweather_info);

  if (gcal_timer_is_running (self->duration_timer))
    gcal_timer_reset (self->duration_timer);

  schedule_midnight (self);
}



/*************
 * < public >
 *************/

/**
 * gcal_weather_service_new:
 * @max_days:       mumber of days to fetch forecasts for.
 * @check_interval_new:   timeout used when fetching new weather reports.
 * @check_interval_renew: timeout used to update valid weather reports.
 *
 * Creates a new #GcalWeatherService. This service listens
 * to location and weather changes and reports them.
 *
 * Returns: (transfer full): A newly created #GcalWeatherService.
 */
GcalWeatherService *
gcal_weather_service_new (GTimeZone *time_zone,
                          guint      max_days,
                          guint      check_interval_new,
                          guint      check_interval_renew,
                          gint64     valid_timespan)
{
  return g_object_new (GCAL_TYPE_WEATHER_SERVICE,
                       "time-zone", time_zone,
                       "max-days", max_days,
                       "check-interval-new", check_interval_new,
                       "check-interval-renew", check_interval_renew,
                       "valid-timespan", valid_timespan,
                       NULL);
}



/**
 * gcal_weather_service_run:
 * @self: The #GcalWeatherService instance.
 * @location: (nullable): A fixed location or %NULL to use Gclue.
 *
 * Starts to monitor location and weather changes.
 * Use ::weather-changed to catch responses.
 */
void
gcal_weather_service_run (GcalWeatherService *self,
                          GWeatherLocation   *location)
{
  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));

  g_debug ("Start weather service");

  if (self->location_service_running || self->weather_service_running)
    gcal_weather_service_stop (self);

  if (location == NULL)
    {
      /* Start location and weather service: */
      self->location_service_running = TRUE;
      self->weather_service_running = TRUE;
      g_cancellable_cancel (self->location_cancellable);
      g_cancellable_reset (self->location_cancellable);
      gclue_simple_new (DESKTOP_FILE_NAME,
                        GCLUE_ACCURACY_LEVEL_EXACT,
                        self->location_cancellable,
                        (GAsyncReadyCallback) on_gclue_simple_creation,
                        g_object_ref (self));
    }
  else
    {
      /* Use the given location to retrieve weather information: */
      self->location_service_running = FALSE;
      self->weather_service_running = TRUE;

      /*_update_location starts timer if necessary */
      gcal_weather_service_update_location (self, location);
    }
}



/**
 * gcal_weather_service_stop:
 * @self: The #GcalWeatherService instance.
 *
 * Stops the service. Returns gracefully if service is
 * not running.
 */
void
gcal_weather_service_stop (GcalWeatherService *self)
{
  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));

  g_debug ("Stop weather service");

  if (!self->location_service_running && !self->weather_service_running)
    return ;

  self->location_service_running = FALSE;
  self->weather_service_running = FALSE;

  /* Notify all listeners about unknown location: */
  gcal_weather_service_update_location (self, NULL);

  if (self->location_service == NULL)
    {
      /* location service is under construction. Cancel creation. */
      g_cancellable_cancel (self->location_cancellable);
    }
  else
    {
      GClueClient *client; /* unowned */

      client = gclue_simple_get_client (self->location_service);

      gclue_client_call_stop (client,
                              NULL,
                              (GAsyncReadyCallback) on_gclue_client_stop,
                              g_steal_pointer (&self->location_service));
    }
}



/**
 * gcal_weather_service_get_max_days:
 * @self: The #GcalWeatherService instance.
 *
 * Getter for #GcalWeatherService:max-days.
 */
guint
gcal_weather_service_get_max_days (GcalWeatherService *self)
{
  g_return_val_if_fail (GCAL_IS_WEATHER_SERVICE (self), 0);

  return self->max_days;
}



/**
 * gcal_weather_service_get_valid_timespan:
 * @self: The #GcalWeatherService instance.
 *
 * Getter for #GcalWeatherService:valid-interval.
 */
gint64
gcal_weather_service_get_valid_timespan (GcalWeatherService *self)
{
  g_return_val_if_fail (GCAL_IS_WEATHER_SERVICE (self), 0);
  return self->valid_timespan;
}



/**
 * gcal_weather_service_get_time_zone:
 * @self: The #GcalWeatherService instance.
 *
 * Getter for #GcalWeatherService:time-zone.
 */
GTimeZone*
gcal_weather_service_get_time_zone (GcalWeatherService *self)
{
  g_return_val_if_fail (self != NULL, NULL);
  return self->time_zone;
}



/**
 * gcal_weather_service_set_time_zone:
 * @self: The #GcalWeatherService instance.
 * @days: Number of days.
 *
 * Setter for #GcalWeatherInfos:time-zone.
 */
void
gcal_weather_service_set_time_zone (GcalWeatherService *self,
                                    GTimeZone          *value)
{
  g_return_if_fail (self != NULL);

  if (self->time_zone != value)
    {
      if (self->time_zone != NULL)
        {
          g_time_zone_unref (self->time_zone);
          self->time_zone = NULL;
        }

      if (value != NULL)
        self->time_zone = g_time_zone_ref (value);

      /* make sure we provide correct weather infos */
      gweather_info_update (self->gweather_info);

      /* make sure midnight is timed correctly: */
      schedule_midnight (self);

      g_object_notify ((GObject*) self, "time-zone");
    }
}




/**
 * gcal_weather_service_get_check_interval_new:
 * @self: The #GcalWeatherService instance.
 *
 * Getter for #GcalWeatherService:check-interval-new.
 */
guint
gcal_weather_service_get_check_interval_new (GcalWeatherService *self)
{
  g_return_val_if_fail (GCAL_IS_WEATHER_SERVICE (self), 0);

  return self->check_interval_new;
}



/**
 * gcal_weather_service_get_check_interval_renew:
 * @self: The #GcalWeatherService instance.
 *
 * Getter for #GcalWeatherService:check-interval-renew.
 */
guint
gcal_weather_service_get_check_interval_renew (GcalWeatherService *self)
{
  g_return_val_if_fail (GCAL_IS_WEATHER_SERVICE (self), 0);

  return self->check_interval_renew;
}



/**
 * gcal_weather_service_get_weather_infos:
 * @self: The #GcalWeatherService instance.
 *
 * Returns: (transfer none): list of known weather reports.
 */
GSList*
gcal_weather_service_get_weather_infos (GcalWeatherService *self)
{
  g_return_val_if_fail (GCAL_IS_WEATHER_SERVICE (self), NULL);

  return self->weather_infos;
}



/**
 * gcal_weather_service_get_attribution:
 * @self: The #GcalWeatherService instance.
 *
 * Returns weather service attribution.
 *
 * Returns: (nullable) (transfer none): Text to display.
 */
const gchar*
gcal_weather_service_get_attribution (GcalWeatherService *self)
{
  g_return_val_if_fail (GCAL_IS_WEATHER_SERVICE (self), NULL);

  if (self->gweather_info != NULL)
    return gweather_info_get_attribution (self->gweather_info);

  return NULL;
}



/**
 * gcal_weather_service_update:
 * @self: The #GcalWeatherService instance.
 *
 * Tries to update weather reports.
 */
void
gcal_weather_service_update (GcalWeatherService *self)
{
  g_return_if_fail (GCAL_IS_WEATHER_SERVICE (self));

  if (self->gweather_info != NULL)
    {
      gweather_info_update (self->gweather_info);
      update_timeout_interval (self);
      if (gcal_timer_is_running (self->duration_timer))
        gcal_timer_reset (self->duration_timer);
    }
}
