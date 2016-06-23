<?xml version="1.0" encoding="UTF-8" ?>
<xsl:stylesheet version="1.0"
                xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <!-- Add a table of contents to index.html -->
  <xsl:output method="html" doctype-system="about:legacy-compat"/>

  <!-- Attach @id to headings -->
  <xsl:template match="div[@class='subsection']/h2">
    <h2>
      <xsl:attribute name="id">
        <xsl:value-of select="."/>
      </xsl:attribute>
      <xsl:value-of select="."/>
    </h2>
  </xsl:template>

  <!-- Insert table of contents -->
  <xsl:template match="div[@class='toc']">
    <div class="toc">
      <xsl:for-each select="//div[@class='subsection']/h2">
        <i class="link-sec">
          <a class="link-sec">
            <xsl:attribute name="href">
              #<xsl:value-of select="."/>
            </xsl:attribute>
            <xsl:value-of select="."/>
            <xsl:value-of select="id(.)"/>
          </a>
        </i>
        <xsl:if test="position() != last()">
          <xsl:text> · </xsl:text>
        </xsl:if>
      </xsl:for-each>
    </div>
  </xsl:template>

  <!-- Rebuild html with table of contents added -->
  <xsl:template match="@*|node()">
    <xsl:copy>
      <xsl:apply-templates select="@*|node()">
      </xsl:apply-templates>
    </xsl:copy>
  </xsl:template>

</xsl:stylesheet>
