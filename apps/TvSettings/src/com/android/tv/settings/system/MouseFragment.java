/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.tv.settings.system;

import android.content.Context;
import android.os.Bundle;
import android.os.SystemProperties;
import android.support.v17.preference.LeanbackPreferenceFragment;
import android.support.v7.preference.ListPreference;
import android.support.v7.preference.Preference;
import android.support.v7.preference.PreferenceScreen;
import android.util.Log;

import com.android.tv.settings.R;

public class MouseFragment extends LeanbackPreferenceFragment {
    private static final String TAG = "MouseFragment";

    private static final String PROPERTY_MOUSE_RIGHT_CLICK_BACK =
            "persist.sys.mouse.rightasback";

    private static final String VALUE_DEFAULT = "default";
    private static final String VALUE_BACK = "back";

    public static MouseFragment newInstance() {
        return new MouseFragment();
    }

    @Override
    public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
        final Context preferenceContext =
                getPreferenceManager().getContext();

        final PreferenceScreen screen =
                getPreferenceManager().createPreferenceScreen(preferenceContext);
        screen.setTitle(R.string.system_mouse);

        final ListPreference rightClickBehavior =
                new ListPreference(preferenceContext);

        rightClickBehavior.setPersistent(false);
        rightClickBehavior.setTitle(
                R.string.mouse_right_click_behavior);
        rightClickBehavior.setDialogTitle(
                R.string.mouse_right_click_behavior);
        rightClickBehavior.setSummary("%s");
        rightClickBehavior.setKey("mouseRightClickBehavior");
        rightClickBehavior.setEntries(
                R.array.mouse_right_click_behavior_entries);
        rightClickBehavior.setEntryValues(
                R.array.mouse_right_click_behavior_values);

        // Read the current behaviour from the persistent system property.
        final boolean useBack = SystemProperties.getBoolean(
                PROPERTY_MOUSE_RIGHT_CLICK_BACK, false);

        rightClickBehavior.setValue(
                useBack ? VALUE_BACK : VALUE_DEFAULT);

        // Store the selected behaviour as a boolean system property.
        rightClickBehavior.setOnPreferenceChangeListener(
                new Preference.OnPreferenceChangeListener() {
                    @Override
                    public boolean onPreferenceChange(
                            Preference preference, Object newValue) {
                        final boolean back;

                        if (VALUE_BACK.equals(newValue)) {
                            back = true;
                        } else if (VALUE_DEFAULT.equals(newValue)) {
                            back = false;
                        } else {
                            return false;
                        }

                        try {
                            SystemProperties.set(
                                    PROPERTY_MOUSE_RIGHT_CLICK_BACK,
                                    Boolean.toString(back));
                            return true;
                        } catch (RuntimeException e) {
                            Log.e(TAG,
                                    "Failed to set mouse right-click behaviour",
                                    e);
                            return false;
                        }
                    }
                });

        screen.addPreference(rightClickBehavior);
        setPreferenceScreen(screen);
    }
}