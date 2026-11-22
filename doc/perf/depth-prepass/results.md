# Timing evidence

All times are milliseconds. A is fresh main; B is the tested bound change. Values are means of run means, followed by their minimum–maximum range. These are descriptive distributions, not confidence intervals. Each run retains GPU and Frame interval n/mean/p50/p95/p99/max, every prepass interval, input hashes, actual routes, phase coverage and shifted-cost means in [evidence.json](evidence.json). No percentile sums or overlapping queue totals are presented.

## pascal-primary

| Case (A/B runs) | GPU A | GPU B | Full prepass chain A | Chain B | Frame interval A | Frame interval B |
| --- | --- | --- | --- | --- | --- | --- |
| club_nrt (5/3) | 61.995 [59.600–67.672] | 57.547 [57.072–57.835] | 2.949 [2.935–2.955] | 2.935 [2.921–2.943] | 63.358 [60.936–68.978] | 58.881 [58.386–59.153] |
| house_nrt (5/3) | 28.276 [26.417–29.433] | 24.069 [23.472–24.643] | 4.921 [4.902–4.956] | 1.928 [1.924–1.932] | 28.392 [26.533–29.540] | 24.205 [23.626–24.763] |
| street_nrt (5/3) | 32.771 [28.300–34.610] | 31.301 [30.747–32.254] | 0.431 [0.423–0.444] | 0.380 [0.376–0.385] | 33.889 [29.421–35.721] | 32.418 [31.868–33.368] |

| Case | Raster A→B | Setup/cull A→B | G-buffer A→B | Deferred graphics A→B | Deferred async A→B | AO graphics A→B | AO async A→B | Alpha A→B | Pre-skin A→B | Probes A→B |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| club_nrt | 2.6310→2.6162 | 0.3184→0.3191 | 4.2053→4.1255 | 25.9111→22.4269 | 0.0000→0.0000 | 1.3859→1.3760 | 0.0000→0.0000 | 10.7300→9.9158 | 1.3041→1.2967 | 0.2235→0.2131 |
| house_nrt | 4.1449→1.1551 | 0.7758→0.7727 | 2.0662→2.0700 | 3.9455→3.8732 | 0.0000→0.0000 | 1.2958→1.3146 | 0.0000→0.0000 | 6.7019→5.8389 | 0.0000→0.0000 | 0.5990→0.5996 |
| street_nrt | 0.1046→0.0507 | 0.3261→0.3296 | 0.3931→0.3719 | 2.8820→3.8375 | 0.0000→0.0000 | 0.8289→0.8512 | 0.0000→0.0000 | 16.7398→14.4072 | 0.0000→0.0000 | 0.2164→0.2115 |

| Case | Per-run GPU means A | Per-run GPU means B | Per-run chain means A | Per-run chain means B |
| --- | --- | --- | --- | --- |
| club_nrt | 67.6719, 60.9059, 60.9068, 60.8892, 59.5997 | 57.8352, 57.0720, 57.7329 | 2.9499, 2.9347, 2.9546, 2.9539, 2.9535 | 2.9430, 2.9208, 2.9422 |
| house_nrt | 29.4329, 29.3785, 26.4170, 27.3545, 28.7986 | 23.4718, 24.6434, 24.0929 | 4.9030, 4.9023, 4.9133, 4.9563, 4.9285 | 1.9245, 1.9318, 1.9272 |
| street_nrt | 34.2710, 34.6103, 28.2998, 34.3258, 32.3470 | 32.2542, 30.9022, 30.7472 | 0.4274, 0.4283, 0.4230, 0.4303, 0.4443 | 0.3796, 0.3761, 0.3853 |

| Case | Interleaved B minus adjacent A GPU means |
| --- | --- |
| club_nrt | -3.0712, -3.8260, -2.5116 |
| house_nrt | -4.4259, -2.2424, -3.9836 |
| street_nrt | +0.7991, -0.4106, -2.5892 |

## pascal-quiet

| Case (A/B runs) | GPU A | GPU B | Full prepass chain A | Chain B | Frame interval A | Frame interval B |
| --- | --- | --- | --- | --- | --- | --- |
| house_quiet (3/3) | 27.443 [25.829–28.806] | 23.535 [23.389–23.696] | 4.923 [4.903–4.954] | 1.925 [1.918–1.935] | 27.571 [25.968–28.920] | 23.673 [23.522–23.826] |

| Case | Raster A→B | Setup/cull A→B | G-buffer A→B | Deferred graphics A→B | Deferred async A→B | AO graphics A→B | AO async A→B | Alpha A→B | Pre-skin A→B | Probes A→B |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| house_quiet | 4.1404→1.1556 | 0.7822→0.7693 | 2.0507→2.0638 | 3.8581→3.5501 | 0.0000→0.0000 | 1.2683→1.2597 | 0.0000→0.0000 | 5.8256→5.8406 | 0.0000→0.0000 | 0.6033→0.6050 |

| Case | Per-run GPU means A | Per-run GPU means B | Per-run chain means A | Per-run chain means B |
| --- | --- | --- | --- | --- |
| house_quiet | 25.8288, 27.6948, 28.8058 | 23.5204, 23.6964, 23.3889 | 4.9025, 4.9539, 4.9113 | 1.9213, 1.9354, 1.9180 |

| Case | Interleaved B minus adjacent A GPU means |
| --- | --- |
| house_quiet | -2.3084, -3.9985, -5.4169 |

## pascal-equal

| Case (A/B runs) | GPU A | GPU B | Full prepass chain A | Chain B | Frame interval A | Frame interval B |
| --- | --- | --- | --- | --- | --- | --- |
| club_equal (3/3) | 59.165 [59.021–59.307] | 58.313 [57.660–59.191] | 3.907 [3.889–3.920] | 3.896 [3.879–3.908] | 60.497 [60.383–60.607] | 59.649 [58.997–60.529] |

| Case | Raster A→B | Setup/cull A→B | G-buffer A→B | Deferred graphics A→B | Deferred async A→B | AO graphics A→B | AO async A→B | Alpha A→B | Pre-skin A→B | Probes A→B |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| club_equal | 3.4690→3.4560 | 0.4377→0.4399 | 4.0399→3.9100 | 23.2874→22.6745 | 0.0000→0.0000 | 0.9497→0.9459 | 0.0000→0.0000 | 9.9005→9.8078 | 1.2930→1.2893 | 0.2217→0.2204 |

| Case | Per-run GPU means A | Per-run GPU means B | Per-run chain means A | Per-run chain means B |
| --- | --- | --- | --- | --- |
| club_equal | 59.3073, 59.0211, 59.1654 | 58.0893, 57.6596, 59.1912 | 3.9101, 3.8895, 3.9204 | 3.9077, 3.8789, 3.9010 |

| Case | Interleaved B minus adjacent A GPU means |
| --- | --- |
| club_equal | -1.2180, -1.3615, +0.0257 |

## amd-accepted

| Case (A/B runs) | GPU A | GPU B | Full prepass chain A | Chain B | Frame interval A | Frame interval B |
| --- | --- | --- | --- | --- | --- | --- |
| club_nrt (5/3) | 13.957 [13.930–13.986] | 13.930 [13.911–13.942] | 0.752 [0.750–0.756] | 0.751 [0.749–0.752] | 15.545 [15.486–15.703] | 15.489 [15.398–15.540] |
| club_rt (5/3) | 46.140 [42.226–51.499] | 42.704 [42.545–42.791] | 2.438 [2.426–2.447] | 2.437 [2.433–2.444] | 47.456 [43.544–52.800] | 44.043 [43.909–44.117] |
| house_nrt (3/3) | 5.126 [5.112–5.138] | 5.134 [5.129–5.144] | 0.449 [0.449–0.450] | 0.450 [0.449–0.452] | 5.339 [5.315–5.353] | 5.342 [5.336–5.353] |
| house_rt (5/3) | 20.466 [20.273–20.754] | 19.856 [18.944–20.346] | 0.945 [0.937–0.951] | 0.939 [0.935–0.945] | 20.684 [20.495–20.927] | 20.105 [19.174–20.616] |

| Case | Raster A→B | Setup/cull A→B | G-buffer A→B | Deferred graphics A→B | Deferred async A→B | AO graphics A→B | AO async A→B | Alpha A→B | Pre-skin A→B | Probes A→B |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| club_nrt | 0.7381→0.7370 | 0.0143→0.0141 | 1.3546→1.3509 | 4.5704→4.5632 | 0.0000→0.0000 | 0.0000→0.0000 | 0.2486→0.2489 | 2.8625→2.8566 | 0.4568→0.4553 | 0.0007→0.0007 |
| club_rt | 2.4057→2.4051 | 0.0324→0.0320 | 0.9793→0.9857 | 4.2238→4.2233 | 17.2901→17.1215 | 0.0000→0.0000 | 0.3238→0.3222 | 14.8185→11.5416 | 0.4576→0.4607 | 0.0009→0.0009 |
| house_nrt | 0.2327→0.2332 | 0.2166→0.2170 | 0.5016→0.5035 | 1.7119→1.7156 | 0.0000→0.0000 | 0.0000→0.0000 | 0.2437→0.2429 | 0.1216→0.1214 | 0.0000→0.0000 | 0.0006→0.0006 |
| house_rt | 0.2536→0.2518 | 0.6914→0.6873 | 0.3054→0.3038 | 2.8283→2.3759 | 8.1853→8.1016 | 0.0000→0.0000 | 0.3166→0.3135 | 1.9084→1.8834 | 0.0000→0.0000 | 0.0007→0.0007 |

| Case | Per-run GPU means A | Per-run GPU means B | Per-run chain means A | Per-run chain means B |
| --- | --- | --- | --- | --- |
| club_nrt | 13.9862, 13.9752, 13.9298, 13.9388, 13.9530 | 13.9111, 13.9365, 13.9419 | 0.7556, 0.7543, 0.7510, 0.7503, 0.7511 | 0.7495, 0.7514, 0.7524 |
| club_rt | 51.4533, 51.4994, 42.7222, 42.8007, 42.2256 | 42.5452, 42.7767, 42.7913 | 2.4420, 2.4473, 2.4369, 2.4391, 2.4257 | 2.4344, 2.4326, 2.4441 |
| house_nrt | 5.1383, 5.1284, 5.1124 | 5.1439, 5.1294, 5.1286 | 0.4503, 0.4492, 0.4485 | 0.4516, 0.4499, 0.4490 |
| house_rt | 20.7542, 20.6659, 20.3446, 20.2897, 20.2733 | 18.9437, 20.3462, 20.2769 | 0.9509, 0.9495, 0.9433, 0.9441, 0.9371 | 0.9372, 0.9447, 0.9354 |

| Case | Interleaved B minus adjacent A GPU means |
| --- | --- |
| club_nrt | -0.0414, +0.0022, -0.0040 |
| club_rt | -4.5656, +0.0152, +0.2782 |
| house_nrt | +0.0056, -0.0040, +0.0082 |
| house_rt | -1.5615, +0.0291, -0.0046 |

## amd-equal

| Case (A/B runs) | GPU A | GPU B | Full prepass chain A | Chain B | Frame interval A | Frame interval B |
| --- | --- | --- | --- | --- | --- | --- |
| club_equal (3/3) | 14.037 [13.379–14.381] | 13.255 [13.236–13.275] | 0.947 [0.940–0.959] | 0.946 [0.943–0.949] | 15.487 [14.730–15.923] | 14.705 [14.677–14.724] |

| Case | Raster A→B | Setup/cull A→B | G-buffer A→B | Deferred graphics A→B | Deferred async A→B | AO graphics A→B | AO async A→B | Alpha A→B | Pre-skin A→B | Probes A→B |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| club_equal | 0.9138→0.9112 | 0.0335→0.0349 | 0.9561→0.9566 | 0.0000→0.0000 | 4.1986→4.2080 | 0.0000→0.0000 | 0.2566→0.2543 | 2.3994→1.6285 | 0.4554→0.4546 | 0.0714→0.0691 |

| Case | Per-run GPU means A | Per-run GPU means B | Per-run chain means A | Per-run chain means B |
| --- | --- | --- | --- | --- |
| club_equal | 14.3524, 14.3805, 13.3789 | 13.2359, 13.2536, 13.2745 | 0.9405, 0.9422, 0.9593 | 0.9458, 0.9433, 0.9493 |

| Case | Interleaved B minus adjacent A GPU means |
| --- | --- |
| club_equal | -1.1165, -1.1269, -0.1044 |

Pairing is B1 versus mean(A2,A3), B2 versus mean(A3,A4), B3 versus mean(A4,A5) for primary cases. AMD house nonRT has rejected A1/A2; B1 uses the remaining adjacent A3. Quiet/EQUAL use Ai/Bi. Runs are ordered by identity suffix in the tables; raw launch order is documented in reproduction. Zero for an optional group means no matching measured mark, not proof that the feature is free.

## Image controls

All complete stills have 240 samples; Native AA, output=render 1919×1080 Pascal and 1920×1080 AMD. Luminance errors and temporal standard deviation use display-space Rec.709 values scaled to 0–255. A/A and A/B errors use the same A2 reference. Noise uses a common reference edge mask. Raw images and crops remain private. Full numeric detail/SSIM/edge and noise percentiles are in [image-evidence.json](image-evidence.json).

| Campaign/case | A/A mean-error range | A/B mean-error range | A noise mean-std range | B noise mean-std range |
| --- | --- | --- | --- | --- |
| pascal-all-image-metrics/club_nrt | 0.003344–0.005306 | 0.005820–0.012145 | 0.705028–0.705268 | 0.705182–0.705200 |
| pascal-all-image-metrics/street_nrt | 0.042356–0.058993 | 0.031551–0.052691 | 3.366277–3.366911 | 3.366503–3.367067 |
| pascal-equal-image-metrics/club_equal | 0.006188–0.011734 | 0.002859–0.003974 | 0.705069–0.705154 | 0.704987–0.705092 |
| amd-image-metrics/club_nrt | 0.003014–0.010994 | 0.004775–0.009399 | 0.626324–0.626498 | 0.626401–0.626446 |
| amd-image-metrics/club_rt | 0.258338–0.440772 | 0.265660–0.428184 | 2.544962–2.599813 | 2.542042–2.554197 |
| amd-equal-image-metrics/club_equal | 0.003591–0.028362 | 0.006611–0.013846 | 0.626399–0.636815 | 0.630913–0.632870 |

Mean images are not bit-identical. Pascal club A/B global mean error exceeds the A/A range, although the absolute error is at most 0.012145/255; its temporal noise stays inside the A/A range. Pascal street B mean-std is at most 0.000156/255 above the A/A maximum, and strict-EQUAL B at most 0.000082/255 below its A/A minimum. These tiny deviations are disclosed rather than called exact equality. AMD RT A/B error and noise track the substantial existing A/A variability; no new visible geometry, alpha boundary or noise defect was resolved in inspected means and worst-error crops. This supports unchanged visible output within finite temporal controls; it cannot establish bitwise equivalence or certify owner EEP/probes, disocclusion trajectories, DLSS/RR or VR. The submitted-draw equivalence argument is separate and stronger for this specific API-limit change.
