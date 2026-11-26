// Custom vector Shift Engine emblem view: geometric dark crest with the
// winding S-road ribbon and crimson-magenta apex pin dot (#E8195E).
package net.shiftengine.drive;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Shader;
import android.view.View;

public class ShiftLogoView extends View {
    private static final int COL_CREST_TOP = Color.parseColor("#232838");
    private static final int COL_CREST_BOT = Color.parseColor("#131722");
    private static final int COL_BORDER = Color.parseColor("#384159");
    private static final int COL_ACCENT = Color.parseColor("#E8195E");
    private static final int COL_ACCENT_HI = Color.parseColor("#FF3B7F");
    private static final int COL_ACCENT_GLOW = Color.parseColor("#44E8195E");
    private static final int COL_ROAD_BASE = Color.parseColor("#2B3246");

    private final Paint mCrestFill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mCrestStroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mRoadShadow = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mRoadPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mGlowPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mDotPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mDotCorePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path mCrestPath = new Path();
    private final Path mRoadPath = new Path();

    public ShiftLogoView(Context context) {
        super(context);

        mCrestFill.setStyle(Paint.Style.FILL);

        mCrestStroke.setStyle(Paint.Style.STROKE);
        mCrestStroke.setColor(COL_BORDER);
        mCrestStroke.setStrokeJoin(Paint.Join.ROUND);

        mRoadShadow.setStyle(Paint.Style.STROKE);
        mRoadShadow.setColor(COL_ROAD_BASE);
        mRoadShadow.setStrokeCap(Paint.Cap.ROUND);
        mRoadShadow.setStrokeJoin(Paint.Join.ROUND);

        mRoadPaint.setStyle(Paint.Style.STROKE);
        mRoadPaint.setStrokeCap(Paint.Cap.ROUND);
        mRoadPaint.setStrokeJoin(Paint.Join.ROUND);

        mGlowPaint.setStyle(Paint.Style.FILL);
        mGlowPaint.setColor(COL_ACCENT_GLOW);

        mDotPaint.setStyle(Paint.Style.FILL);
        mDotPaint.setColor(COL_ACCENT_HI);

        mDotCorePaint.setStyle(Paint.Style.FILL);
        mDotCorePaint.setColor(Color.parseColor("#FFE4EE"));
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        float s = Math.min(w, h);
        if (s <= 0f) return;
        float ox = (w - s) * 0.5f;
        float oy = (h - s) * 0.5f;

        mCrestFill.setShader(new LinearGradient(
                ox, oy, ox, oy + s, COL_CREST_TOP, COL_CREST_BOT, Shader.TileMode.CLAMP));
        mRoadPaint.setShader(new LinearGradient(
                ox + s * 0.25f, oy + s * 0.78f, ox + s * 0.75f, oy + s * 0.22f,
                COL_ACCENT, COL_ACCENT_HI, Shader.TileMode.CLAMP));

        // Geometric hexagonal shield crest
        mCrestPath.reset();
        float pad = s * 0.06f;
        float x0 = ox + pad;
        float x1 = ox + s - pad;
        float cx = ox + s * 0.5f;
        float yTop = oy + pad;
        float yUpper = oy + s * 0.24f;
        float yLower = oy + s * 0.72f;
        float yBot = oy + s - pad;

        mCrestPath.moveTo(cx, yTop);
        mCrestPath.lineTo(x1, yUpper);
        mCrestPath.lineTo(x1, yLower);
        mCrestPath.lineTo(cx, yBot);
        mCrestPath.lineTo(x0, yLower);
        mCrestPath.lineTo(x0, yUpper);
        mCrestPath.close();

        // Winding S-road inside the crest rising toward the top-right apex pin
        mRoadPath.reset();
        mRoadPath.moveTo(ox + s * 0.30f, oy + s * 0.76f);
        mRoadPath.cubicTo(
                ox + s * 0.66f, oy + s * 0.72f,
                ox + s * 0.70f, oy + s * 0.55f,
                ox + s * 0.49f, oy + s * 0.50f);
        mRoadPath.cubicTo(
                ox + s * 0.28f, oy + s * 0.45f,
                ox + s * 0.34f, oy + s * 0.29f,
                ox + s * 0.66f, oy + s * 0.26f);

        mCrestStroke.setStrokeWidth(Math.max(1.5f, s * 0.042f));
        mRoadShadow.setStrokeWidth(Math.max(3.5f, s * 0.145f));
        mRoadPaint.setStrokeWidth(Math.max(2.2f, s * 0.095f));
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float s = Math.min(getWidth(), getHeight());
        if (s <= 0f) return;
        float ox = (getWidth() - s) * 0.5f;
        float oy = (getHeight() - s) * 0.5f;

        canvas.drawPath(mCrestPath, mCrestFill);
        canvas.drawPath(mCrestPath, mCrestStroke);

        canvas.drawPath(mRoadPath, mRoadShadow);
        canvas.drawPath(mRoadPath, mRoadPaint);

        // Apex route pin dot
        float pinX = ox + s * 0.67f;
        float pinY = oy + s * 0.26f;
        canvas.drawCircle(pinX, pinY, s * 0.135f, mGlowPaint);
        canvas.drawCircle(pinX, pinY, s * 0.072f, mDotPaint);
        canvas.drawCircle(pinX, pinY, s * 0.026f, mDotCorePaint);
    }
}
