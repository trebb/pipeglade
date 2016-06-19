<?xml version="1.0" encoding="UTF-8" ?>
<xsl:stylesheet version="1.0"
                xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <!-- Add a table of contents to index.html -->
  <xsl:output method="html" doctype-system="about:legacy-compat"/>


  <!-- Attach id attributes to headings -->

  <xsl:template match="div[@class='subsection']/h2" mode="main">
    <h2>
      <xsl:attribute name="id">
        <xsl:value-of select='.'/>
      </xsl:attribute>
      <xsl:value-of select="."/>
    </h2>
  </xsl:template>


  <!-- Collect table of contents' entries -->

  <xsl:template match="div[@class='subsection']/h2" mode="toc">
    <i class="link-sec">
      <a class="link-sec">
        <xsl:attribute name="href">
          #<xsl:value-of select="."/>
        </xsl:attribute>
        <xsl:value-of select="."/>
        <xsl:value-of select="id(.)"/>
      </a>
    </i>
    <xsl:text> </xsl:text>
  </xsl:template>

  <xsl:template match="text()" mode="toc"/>


  <!-- Insert table of contents -->

  <xsl:template match="div[@class='toc']" mode="main">
    <xsl:param name="headings"/>
    <div class="toc">
      <xsl:copy-of select="$headings"/>
    </div>
  </xsl:template>


  <!-- Build html with table of contents added -->

  <xsl:template match="@*|node()">
    <xsl:apply-templates mode="main">
      <xsl:with-param name="headings">
        <xsl:apply-templates mode="toc"/>
      </xsl:with-param>
    </xsl:apply-templates>
  </xsl:template>

  <xsl:template match="@*|node()" mode="main">
    <xsl:param name="headings"/>
    <xsl:copy>
      <xsl:apply-templates select="@*|node()" mode="main">
        <xsl:with-param name="headings" select="$headings"/>
      </xsl:apply-templates>
    </xsl:copy>
  </xsl:template>

</xsl:stylesheet>
