# Claims log — append-only

Every "ships", "complete", or quality-pass claim about a pipeline goes here, written by `tests/quality_gates/run_gate.py --claim` (never by hand). Run-hash is the receipt. If a claim isn't in this file with a matching `runs/<hash>/run.json`, it doesn't count.

Format:
```
- YYYY-MM-DD HH:MM  pipeline=`<full-pipeline-name>`  run=<hash>  worst_lpips=<v>  worst_image=<id>  visual_description="<inspection sentence>"
```

`visual_description` must (a) be at least 6 words and (b) contain a concrete noun from the gate's whitelist (rocks, sky, edge, blockiness, haze, noise, detail, texture, shadow, highlight, crosshatch, smooth, ringing, color). "Looks fine" doesn't pass. The runner refuses to log a sentence that doesn't.

Failed gate runs are NOT logged here — they live in their `runs/<hash>/run.json`.

## Receipt scope

The eight committed baseline receipts remain the current required signoffs:
`b44fa841c05c9bff`, `5c3cce4c472d4197`, `8864c12ec0b6ce14`,
`40c393acc2ad89bc`, `8941d5ac9b33f826`, `f2f3e652f2d26dbf`,
`9837b797660627aa`, and `54f001717c547574`.

Entries `5e0b4c751d7a26eb` and `5e7b79b5678fdf62` below are historical:
their receipts are not committed here, so they are not current signoffs or
current missing-receipt blockers. All recorded entries remain unchanged.
See [Ship Decision](SHIP_DECISION.md) for the bounded approval scope.
Removed experimental receipts remain in the
[pinned snapshot](https://github.com/dcliftreaves/gpr/tree/f13dbe95d3e97c400e8d493ba178dbb037ff3c96/tests/quality_gates/runs).

## 2026-05-26

- 2026-05-26 19:02  pipeline=`codec=ml2_q3+cnn=bibo1x_ane_ml2_q3+demosaic=sips_via_gpr_tools`  run=5e0b4c751d7a26eb  worst_lpips=0.0683  worst_image=Z8Z_6693  visual_description="Z8Z_6693 skin-tone gradient: pipeline matches REF with no visible blockiness or color shift, only subtle texture difference."
- 2026-05-31 07:42  pipeline=`codec=gpr_tools_q3+cnn=bibo1x_ane_gpr_tools_q3+demosaic=sips_via_gpr_tools`  run=b44fa841c05c9bff  worst_lpips=0.0155  worst_image=Z8Z_6693  visual_description="Z8Z_6693 fabric texture and gray edge match REF with no visible blockiness or color shift."
- 2026-05-31 07:43  pipeline=`codec=ml2_q3_l1x2+cnn=bibo1x_ane_ml2_q3+demosaic=sips_via_gpr_tools`  run=5c3cce4c472d4197  worst_lpips=0.0760  worst_image=Z8Z_6693  visual_description="Z8Z_6693 fabric texture stays coherent; gray edge is softer but free of blockiness or ringing."
- 2026-05-31 07:44  pipeline=`codec=sl_q3+cnn=none+demosaic=sips_via_gpr_tools`  run=5e7b79b5678fdf62  worst_lpips=0.1003  worst_image=Z8Z_6693  visual_description="Z8Z_6693 texture and color remain close to REF with no obvious blockiness."
- 2026-05-31 07:44  pipeline=`codec=ml2_q3_dec2+cnn=bibo2x_ane_ml2_q3_dec2_diverse+demosaic=sips_via_gpr_tools`  run=8864c12ec0b6ce14  worst_lpips=0.3433  worst_image=Z8Z_6693  visual_description="Z8Z_6693 texture is smoother than REF, but Bayer detail remains stable without blockiness."
- 2026-06-01 16:27  pipeline=`codec=ml2_q3+cnn=bibo1x_ane_ml2_q3+demosaic=sips_via_gpr_tools`  run=40c393acc2ad89bc  worst_lpips=0.0683  worst_image=Z8Z_6693  visual_description="Z8Z_6693 texture and color stay coherent with no visible blockiness or ringing."
- 2026-06-01 16:28  pipeline=`codec=ml2_q3_l2x2_l1x2+cnn=bibo1x_ane_ml2_q3+demosaic=sips_via_gpr_tools`  run=8941d5ac9b33f826  worst_lpips=0.0794  worst_image=Z8Z_6693  visual_description="Z8Z_6693 fabric texture remains stable with no visible blockiness or color break."
- 2026-06-01 16:29  pipeline=`codec=ml2_q3_l2x2_l1x2_hh1x2+cnn=bibo1x_ane_ml2_q3+demosaic=sips_via_gpr_tools`  run=f2f3e652f2d26dbf  worst_lpips=0.0809  worst_image=Z8Z_6693  visual_description="Z8Z_6693 texture and gray edge remain coherent without visible blockiness or ringing."
- 2026-06-01 16:30  pipeline=`codec=gpr_tools_q8+cnn=none+demosaic=sips_via_gpr_tools`  run=9837b797660627aa  worst_lpips=0.0035  worst_image=Z8Z_6693  visual_description="Z8Z_6693 fabric texture and gray edge match REF without visible blockiness or color shift."
- 2026-06-01 16:31  pipeline=`codec=gpr_tools_q0+cnn=bibo1x_ane_gpr_tools_q3+demosaic=sips_via_gpr_tools`  run=54f001717c547574  worst_lpips=0.0314  worst_image=Z8Z_6693  visual_description="Z8Z_6693 fabric texture and shadow detail remain coherent without visible blockiness."
